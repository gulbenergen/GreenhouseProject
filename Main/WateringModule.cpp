#include "WateringModule.h"
#include "ModuleController.h"

static uint8_t WATER_RELAYS[] = { WATER_RELAYS_PINS }; // объявляем массив пинов реле

void WateringModule::Setup()
{
  // настройка модуля тут

  settings = mainController->GetSettings();

#ifdef USE_WATERING_MANUAL_MODE_DIODE
  blinker.begin(DIODE_WATERING_MANUAL_MODE_PIN, F("WM"));  // настраиваем блинкер на нужный пин
#endif

  workMode = wwmAutomatic; // автоматический режим работы
  dummyAllChannels.WateringTimer = 0; // обнуляем таймер полива для всех каналов
  dummyAllChannels.IsChannelRelayOn = false; // все реле выключены

  lastDOW = -1; // неизвестный день недели
  currentDOW = -1; // ничего не знаем про текущий день недели
  currentHour = -1; // и про текущий час тоже ничего не знаем
  lastAnyChannelActiveFlag = -1; // ещё не собирали активность каналов

  #ifdef USE_DS3231_REALTIME_CLOCK
    bIsRTClockPresent = true; // есть часы реального времени
  #else
    bIsRTClockPresent = false; // нет часов реального времени
  #endif

   #ifdef SAVE_RELAY_STATES
   uint8_t relayCnt = WATER_RELAYS_COUNT/8; // устанавливаем кол-во каналов реле
   if(WATER_RELAYS_COUNT > 8 && WATER_RELAYS_COUNT % 8)
    relayCnt++;

   if(WATER_RELAYS_COUNT < 9)
    relayCnt = 1;
    
   for(uint8_t i=0;i<relayCnt;i++) // добавляем состояния реле (каждый канал - 8 реле)
    State.AddState(StateRelay,i);
   #endif  

    
  // выключаем все реле
  for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
  {
    pinMode(WATER_RELAYS[i],OUTPUT);
    digitalWrite(WATER_RELAYS[i],RELAY_OFF);

    #ifdef SAVE_RELAY_STATES
    uint8_t idx = i/8;
    uint8_t bitNum1 = i % 8;
    OneState* os = State.GetState(StateRelay,idx);
    if(os)
    {
      RelayPair rp = *os;
      uint8_t curRelayStates = rp.Current;
      bitWrite(curRelayStates,bitNum1, dummyAllChannels.IsChannelRelayOn);
      os->Update((void*)&curRelayStates);
    }
    #endif

    // настраиваем все каналы
    wateringChannels[i].IsChannelRelayOn = dummyAllChannels.IsChannelRelayOn;
    wateringChannels[i].WateringTimer = 0;
  } // for

#ifdef USE_PUMP_RELAY
  // выключаем реле насоса  
  pinMode(PUMP_RELAY_PIN,OUTPUT);
  digitalWrite(PUMP_RELAY_PIN,RELAY_OFF);
#endif

    // настраиваем режим работы перед стартом
    uint8_t currentWateringOption = settings->GetWateringOption();
    
    if(currentWateringOption == wateringOFF) // если выключено автоуправление поливом
    {
      workMode = wwmManual; // переходим в ручной режим работы
      #ifdef USE_WATERING_MANUAL_MODE_DIODE
      blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
      #endif
    }
    else
    {
      workMode = wwmAutomatic; // иначе переходим в автоматический режим работы
      #ifdef USE_WATERING_MANUAL_MODE_DIODE
      blinker.blink(); // гасим диод
      #endif
    }
      

}
void WateringModule::UpdateChannel(int8_t channelIdx, WateringChannel* channel, uint16_t dt)
{
   if(!bIsRTClockPresent)
   {
     // в системе нет модуля часов, в таких условиях мы можем работать только в ручном режиме.
     // поэтому в этой ситуации мы ничего не предпринимаем, поскольку автоматически деградируем
     // в ручной режим работы.
     return;
   }
   
     uint8_t weekDays = channelIdx == -1 ? settings->GetWateringWeekDays() : settings->GetChannelWateringWeekDays(channelIdx);
     uint8_t startWateringTime = channelIdx == -1 ? settings->GetStartWateringTime() : settings->GetChannelStartWateringTime(channelIdx);
     uint16_t timeToWatering = channelIdx == -1 ? settings->GetWateringTime() : settings->GetChannelWateringTime(channelIdx); // время полива (в минутах!)


    // проверяем, установлен ли у нас день недели для полива, и настал ли час, с которого можно поливать
    bool canWork = bitRead(weekDays,currentDOW-1) && currentHour >= startWateringTime;
  
    if(!canWork)
     {            
       channel->WateringTimer = 0; // в этот день недели и в этот час работать не можем, однозначно обнуляем таймер полива    
       channel->IsChannelRelayOn = false; // выключаем реле
     }
     else
     {
      // можем работать, смотрим, не вышли ли мы за пределы установленного интервала
 
      if(lastDOW != currentDOW)  // сначала проверяем, не другой ли день недели уже?
      {
        // начался другой день недели, в который мы можем работать. Для одного дня недели у нас установлена
        // продолжительность полива, поэтому, если мы поливали 28 минут вместо 30, например, во вторник, и перешли на среду,
        // то в среду надо полить 32 мин. Поэтому таймер полива переводим в нужный режим:
        // оставляем в нём недополитое время, чтобы учесть, что поливать надо, например, 32 минуты.
  
        //               разница между полным и отработанным временем
        channel->WateringTimer = -((timeToWatering*60000) - channel->WateringTimer); // загоняем в минус, чтобы добавить недостающие минуты к работе
      }
      
      channel->WateringTimer += dt; // прибавляем время работы
  
      // проверяем, можем ли мы ещё работать
      // если полив уже отработал, и юзер прибавит минуту - мы должны поливать ещё минуту,
      // вне зависимости от показания таймера. Поэтому мы при срабатывании условия окончания полива
      // просто отнимаем дельту времени из таймера, таким образом оставляя его застывшим по времени
      // окончания полива
  
      if(channel->WateringTimer > (timeToWatering*60000) + dt) // приплыли, надо выключать полив
      {
        channel->WateringTimer -= dt;// оставляем таймер застывшим на окончании полива, плюс маленькая дельта
        channel->IsChannelRelayOn = false;
      }
      else
        channel->IsChannelRelayOn = true; // ещё можем работать, продолжаем поливать
     } // else

  
}
void WateringModule::HoldChannelState(int8_t channelIdx, WateringChannel* channel)
{
    uint8_t state = channel->IsChannelRelayOn ? RELAY_ON : RELAY_OFF;

    if(channelIdx == -1) // работаем со всеми каналами
    {
      for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
      {
        digitalWrite(WATER_RELAYS[i],state);

         #ifdef SAVE_RELAY_STATES
         uint8_t idx = i/8; // выясняем, какой индекс
         uint8_t bitNum1 = i % 8;
         OneState* os = State.GetState(StateRelay,idx);
         if(os)
         {
          RelayPair rp = *os;
          uint8_t curRelayStates = rp.Current; // получаем текущую маску состояния реле
          bitWrite(curRelayStates,bitNum1, channel->IsChannelRelayOn);
          os->Update((void*)&curRelayStates);
         }
         #endif
      
      }   
      return;
    } // if

    // работаем с одним каналом
    digitalWrite(WATER_RELAYS[channelIdx],state);

     #ifdef SAVE_RELAY_STATES 
     uint8_t idx = channelIdx/8; // выясняем, какой индекс
     uint8_t bitNum1 = channelIdx % 8;
     OneState* os = State.GetState(StateRelay,idx);
     if(os)
     {
      RelayPair rp = *os;
      uint8_t curRelayStates = rp.Current; // получаем текущую маску состояния реле
      bitWrite(curRelayStates,bitNum1, channel->IsChannelRelayOn);
      os->Update((void*)&curRelayStates);
     }
    #endif
    
}

bool WateringModule::IsAnyChannelActive(uint8_t wateringOption)
{  
   if(workMode == wwmManual) // в ручном режиме мы управляем только всеми каналами сразу
    return dummyAllChannels.IsChannelRelayOn; // поэтому смотрим состояние реле на всех каналах

    // в автоматическом режиме мы можем рулить как всеми каналами вместе (wateringOption == wateringWeekDays),
    // так и по отдельности (wateringOption == wateringSeparateChannels). В этом случае надо выяснить, состояние каких каналов
    // смотреть, чтобы понять - активен ли кто-то.

    if(wateringOption == wateringWeekDays)
      return dummyAllChannels.IsChannelRelayOn; // смотрим состояние реле на всех каналах

    // тут мы рулим всеми каналами по отдельности, поэтому надо проверить - включено ли реле на каком-нибудь из каналов
    for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
    {
      if(wateringChannels[i].IsChannelRelayOn)
        return true;
    }

    return false;
}
#ifdef USE_PUMP_RELAY
void WateringModule::HoldPumpState(uint8_t wateringOption)
{
  // поддерживаем состояние реле насоса
    bool bPumpIsOn = false;
    
    if(settings->GetTurnOnPump() == 1) // если мы должны включать насос при поливе на любом из каналов
      bPumpIsOn = IsAnyChannelActive(wateringOption); // то делаем это только тогда, когда полив включен на любом из каналов

    // пишем в реле насоса вкл или выкл в зависимости от настройки "включать насос при поливе"
    uint8_t state = bPumpIsOn ? RELAY_ON : RELAY_OFF;
    digitalWrite(PUMP_RELAY_PIN,state); 
}
#endif

void WateringModule::Update(uint16_t dt)
{ 
#ifdef USE_WATERING_MANUAL_MODE_DIODE
   blinker.update();
#endif
  
   uint8_t wateringOption = settings->GetWateringOption(); // получаем опцию управления поливом

#ifdef USE_PUMP_RELAY
  // держим состояние реле для насоса
  HoldPumpState(wateringOption);
#endif

  #ifdef USE_DS3231_REALTIME_CLOCK

    // обновляем состояние часов
    DS3231Clock watch =  mainController->GetClock();
    DS3231Time t =   watch.getTime();
    
    if(currentDOW == -1) // если мы не сохраняли текущий день недели, то
    {
      currentDOW = t.dayOfWeek; // сохраним его, чтобы потом проверять переход через дни недели
      lastDOW = t.dayOfWeek; // сохраним и как предыдущий день недели
    }

    if(currentDOW != t.dayOfWeek)
    {
      // начался новый день недели, принудительно переходим в автоматический режим работы
      // даже если до этого был включен полив командой от пользователя
      workMode = wwmAutomatic;
    }

    currentDOW = t.dayOfWeek; // сохраняем текущий день недели
    currentHour = t.hour; // сохраняем текущий час
       
  #else

    // модуль часов реального времени не включен в компиляцию, деградируем до ручного режима работы
    settings->SetWateringOption(wateringOFF); // отключим автоматический контроль полива
    workMode = wwmManual; // переходим на ручное управление
    #ifdef USE_WATERING_MANUAL_MODE_DIODE
    blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
    #endif
 
  #endif
  
  if(workMode == wwmAutomatic)
  {
    // автоматический режим работы
 
    // проверяем текущий режим управления каналами полива
    switch(wateringOption)
    {
      case wateringOFF: // автоматическое управление поливом выключено, значит, мы должны перейти в ручной режим работы
          workMode = wwmManual; // переходим в ручной режим работы
          #ifdef USE_WATERING_MANUAL_MODE_DIODE
          blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
          #endif
      break;

      case wateringWeekDays: // // управление поливом по дням недели (все каналы одновременно)
      {
          // обновляем состояние всех каналов - канал станет активным или неактивным после этой проверки
           UpdateChannel(-1,&dummyAllChannels,dt);
           
           // теперь держим текущее состояние реле на всех каналах
           HoldChannelState(-1,&dummyAllChannels);
      }
      break;

      case wateringSeparateChannels: // рулим всеми каналами по отдельности
      {
        for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
        {
          UpdateChannel(i,&(wateringChannels[i]),dt); // обновляем канал
          HoldChannelState(i,&(wateringChannels[i]));  // держим его состояние
        } // for
      }
      break;
      
    } // switch(wateringOption)
  }
  else
  {
    // ручной режим работы, просто сохраняем переданный нам статус реле, все каналы - одновременно.
    // обновлять состояние канала не надо, потому что мы в ручном режиме работы.
      HoldChannelState(-1,&dummyAllChannels);
          
  } // else

  // проверяем, есть ли изменения с момента последнего вызова
  if(lastAnyChannelActiveFlag < 0)
  {
    // ещё не собирали статус, собираем первый раз
    lastAnyChannelActiveFlag = IsAnyChannelActive(wateringOption) ? 1 : 0;

    if(lastAnyChannelActiveFlag)
    {
      // если любой канал активен - значит, полив включили, а по умолчанию он выключен.
      // значит, надо записать в лог
      String mess = lastAnyChannelActiveFlag? STATE_ON : STATE_OFF;
      mainController->Log(this,mess);
    }
  }
  else
  {
    // уже собирали, надо проверить с текущим состоянием
    byte nowAnyChannelActive = IsAnyChannelActive(wateringOption) ? 1 : 0;
    
    if(nowAnyChannelActive != lastAnyChannelActiveFlag)
    {
      lastAnyChannelActiveFlag = nowAnyChannelActive; // сохраняем последний статус, чтобы не дёргать запись в лог лишний раз
      // состояние каналов изменилось, пишем в лог
      String mess = lastAnyChannelActiveFlag ? STATE_ON : STATE_OFF;
      mainController->Log(this,mess);
    }
  } // else

  // обновили все каналы, теперь можно сбросить флаг перехода через день недели
  lastDOW = currentDOW; // сделаем вид, что мы ничего не знаем о переходе на новый день недели.
  // таким образом, код перехода на новый день недели выполнится всего один раз при каждом переходе
  // через день недели.
  
}
bool  WateringModule::ExecCommand(const Command& command, bool wantAnswer)
{
  UNUSED(wantAnswer);
  PublishSingleton = UNKNOWN_COMMAND;

  size_t argsCount = command.GetArgsCount();
    
  if(command.GetType() == ctSET) 
  {   
      if(argsCount < 1) // не хватает параметров
      {
        PublishSingleton = PARAMS_MISSED;
      }
      else
      {
        String which = command.GetArg(0);
        which.toUpperCase();

        if(which == WATER_SETTINGS_COMMAND)
        {
          if(argsCount > 5)
          {
              // парсим параметры
              uint8_t wateringOption = String(command.GetArg(1)).toInt();
              uint8_t wateringWeekDays = String(command.GetArg(2)).toInt();
              uint16_t wateringTime = String(command.GetArg(3)).toInt();
              uint8_t startWateringTime = String(command.GetArg(4)).toInt();
              uint8_t turnOnPump = String(command.GetArg(5)).toInt();
      
              // пишем в настройки
              settings->SetWateringOption(wateringOption);
              settings->SetWateringWeekDays(wateringWeekDays);
              settings->SetWateringTime(wateringTime);
              settings->SetStartWateringTime(startWateringTime);
              settings->SetTurnOnPump(turnOnPump);
      
              // сохраняем настройки
              settings->Save();

              if(wateringOption == wateringOFF) // если выключено автоуправление поливом
              {
                workMode = wwmManual; // переходим в ручной режим работы
                dummyAllChannels.IsChannelRelayOn = false; // принудительно гасим полив на всех каналах
                #ifdef USE_WATERING_MANUAL_MODE_DIODE
                blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
                #endif
              }
              else
              {
                workMode = wwmAutomatic; // иначе переходим в автоматический режим работы
                #ifdef USE_WATERING_MANUAL_MODE_DIODE
                blinker.blink(); // гасим диод
                #endif
              }
      
              
              PublishSingleton.Status = true;
              PublishSingleton = WATER_SETTINGS_COMMAND; 
              PublishSingleton << PARAM_DELIMITER << REG_SUCC;
          } // argsCount > 3
          else
          {
            // не хватает команд
            PublishSingleton = PARAMS_MISSED;
          }
          
        } // WATER_SETTINGS_COMMAND
        else
        if(which == WATER_CHANNEL_SETTINGS) // настройки канала CTSET=WATER|CH_SETT|IDX|WateringDays|WateringTime|StartTime
        {
           if(argsCount > 4)
           {
                uint8_t channelIdx = String(command.GetArg(1)).toInt();
                if(channelIdx < WATER_RELAYS_COUNT)
                {
                  // нормальный индекс
                  uint8_t wDays = String(command.GetArg(2)).toInt();
                  uint16_t wTime = String(command.GetArg(3)).toInt();
                  uint8_t sTime = String(command.GetArg(4)).toInt();
                  
                  settings->SetChannelWateringWeekDays(channelIdx,wDays);
                  settings->SetChannelWateringTime(channelIdx,wTime);
                  settings->SetChannelStartWateringTime(channelIdx,sTime);
                  
                  PublishSingleton.Status = true;
                  PublishSingleton = WATER_CHANNEL_SETTINGS; 
                  PublishSingleton << PARAM_DELIMITER << (command.GetArg(1)) << PARAM_DELIMITER << REG_SUCC;
                 
                }
                else
                {
                  // плохой индекс
                  PublishSingleton = UNKNOWN_COMMAND;
                }
           }
           else
           {
            // не хватает команд
            PublishSingleton = PARAMS_MISSED;            
           }
        }
        else
        if(which == WORK_MODE) // CTSET=WATER|MODE|AUTO, CTSET=WATER|MODE|MANUAL
        {
           // попросили установить режим работы
           String param = command.GetArg(1);
           
           if(param == WM_AUTOMATIC)
           {
             workMode = wwmAutomatic; // переходим в автоматический режим работы
             #ifdef USE_WATERING_MANUAL_MODE_DIODE
             blinker.blink(); // гасим диод
             #endif
           }
           else
           {
            workMode = wwmManual; // переходим на ручной режим работы
            #ifdef USE_WATERING_MANUAL_MODE_DIODE
            blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
            #endif
           }

              PublishSingleton.Status = true;
              PublishSingleton = WORK_MODE; 
              PublishSingleton << PARAM_DELIMITER << param;
        
        } // WORK_MODE
        else 
        if(which == STATE_ON) // попросили включить полив, CTSET=WATER|ON
        {
          if(!command.IsInternal()) // если команда от юзера, то
          {
            workMode = wwmManual; // переходим в ручной режим работы
            #ifdef USE_WATERING_MANUAL_MODE_DIODE
            blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
            #endif
          }
          // если команда не от юзера, а от модуля ALERT, например, то
          // просто выставляя статус реле для всех каналов - мы ничего не добьёмся - 
          // команда проигнорируется, т.к. мы сами обновляем статус каналов.
          // в этом случае - надо переходить на ручное управление, мне кажется.
          // Хотя это - неправильно, должна быть возможность в автоматическом
          // режиме включать/выключать полив из модуля ALERT, без мигания диодом.

          dummyAllChannels.IsChannelRelayOn = true; // включаем реле на всех каналах

          PublishSingleton.Status = true;
          PublishSingleton = STATE_ON;
        } // STATE_ON
        else 
        if(which == STATE_OFF) // попросили выключить полив, CTSET=WATER|OFF
        {
          if(!command.IsInternal()) // если команда от юзера, то
          {
            workMode = wwmManual; // переходим в ручной режим работы
            #ifdef USE_WATERING_MANUAL_MODE_DIODE
            blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
            #endif
          }
          // если команда не от юзера, а от модуля ALERT, например, то
          // просто выставляя статус реле для всех каналов - мы ничего не добьёмся - 
          // команда проигнорируется, т.к. мы сами обновляем статус каналов.
          // в этом случае - надо переходить на ручное управление, мне кажется.
          // Хотя это - неправильно, должна быть возможность в автоматическом
          // режиме включать/выключать полив из модуля ALERT, без мигания диодом.

          dummyAllChannels.IsChannelRelayOn = false; // выключаем реле на всех каналах

          PublishSingleton.Status = true;
          PublishSingleton = STATE_OFF;
          
        } // STATE_OFF        

      } // else
  }
  else
  if(command.GetType() == ctGET) //получить данные
  {    
    if(!argsCount) // нет аргументов, попросили вернуть статус полива
    {
      PublishSingleton.Status = true;
      PublishSingleton = (IsAnyChannelActive(settings->GetWateringOption()) ? STATE_ON : STATE_OFF);
      PublishSingleton << PARAM_DELIMITER << (workMode == wwmAutomatic ? WM_AUTOMATIC : WM_MANUAL);
    }
    else
    {
      String t = command.GetArg(0);
      
        if(t == WATER_SETTINGS_COMMAND) // запросили данные о настройках полива
        {
          PublishSingleton.Status = true;
          PublishSingleton = WATER_SETTINGS_COMMAND; 
          PublishSingleton << PARAM_DELIMITER; 
          PublishSingleton << (settings->GetWateringOption()) << PARAM_DELIMITER;
          PublishSingleton << (settings->GetWateringWeekDays()) << PARAM_DELIMITER;
          PublishSingleton << (settings->GetWateringTime()) << PARAM_DELIMITER;
          PublishSingleton << (settings->GetStartWateringTime()) << PARAM_DELIMITER;
          PublishSingleton << (settings->GetTurnOnPump());
        }
        else
        if(t == WATER_CHANNELS_COUNT_COMMAND)
        {
          PublishSingleton.Status = true;
          PublishSingleton = WATER_CHANNELS_COUNT_COMMAND; 
          PublishSingleton << PARAM_DELIMITER << WATER_RELAYS_COUNT;
          
        }
        else
        if(t == WORK_MODE) // получить режим работы
        {
          PublishSingleton.Status = true;
          PublishSingleton = WORK_MODE; 
          PublishSingleton << PARAM_DELIMITER << (workMode == wwmAutomatic ? WM_AUTOMATIC : WM_MANUAL);
        }
        else
        {
           // команда с аргументами
           if(argsCount > 1)
           {
                t = command.GetArg(0);
    
                if(t == WATER_CHANNEL_SETTINGS)
                {
                  // запросили настройки канала
                  uint8_t idx = String(command.GetArg(1)).toInt();
                  
                  if(idx < WATER_RELAYS_COUNT)
                  {
                    PublishSingleton.Status = true;
                 
                    PublishSingleton = WATER_CHANNEL_SETTINGS; 
                    PublishSingleton << PARAM_DELIMITER << (command.GetArg(1)) << PARAM_DELIMITER 
                    << (settings->GetChannelWateringWeekDays(idx)) << PARAM_DELIMITER
                    << (settings->GetChannelWateringTime(idx)) << PARAM_DELIMITER
                    << (settings->GetChannelStartWateringTime(idx));
                  }
                  else
                  {
                    // плохой индекс
                    PublishSingleton = UNKNOWN_COMMAND;
                  }
                          
                } // if
           } // if
        } // else
    } // else have arguments
  } // if ctGET
 
 // отвечаем на команду
    mainController->Publish(this,command);
    
  return PublishSingleton.Status;
}

