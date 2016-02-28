#ifndef _HUMIDITY_GLOBALS_H
#define _HUMIDITY_GLOBALS_H
typedef struct
{
  bool IsOK; // данные получены?
  uint8_t Humidity; // целое значение влажности
  uint8_t HumidityDecimal; // значение влажности после запятой
  int8_t Temperature; // целое значение температуры
  uint8_t TemperatureDecimal; // значение температуры после запятой
  
} HumidityAnswer; // ответ от датчика

typedef enum
{
  DHT11, // сенсор DHT11
  DHT2x // сенсор DHT21 и старше
  
} HumiditySensorType; // какие сенсоры поддерживаем


#endif