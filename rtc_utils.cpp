#include "rtc_utils.h"
#include "config.h"

time_t calculateNextSend(time_t now, time_t lastSent, int intervalMin){
  struct tm tm_info = *localtime(&now);
  tm_info.tm_sec = 0;

  // Round minutes down to nearest interval
  int remainder = tm_info.tm_min % intervalMin;
  tm_info.tm_min = tm_info.tm_min - remainder + intervalMin; // next interval

  time_t next = mktime(&tm_info);

  if(next <= now) next += intervalMin*60; // ensure strictly in future
  return next;
}


String timeToStr(time_t t){
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
  return String(buf);
}
