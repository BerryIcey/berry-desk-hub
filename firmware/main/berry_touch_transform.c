#include "berry_touch_transform.h"
void berry_touch_transform(meter_rotation_t rotation,uint16_t width,uint16_t height,uint16_t *x,uint16_t *y){
 if(!x || !y || !width || !height)return;
 uint16_t a=*x<width?*x:width-1,b=*y<height?*y:height-1;
 switch(rotation){
 case METER_ROTATION_90:*x=height-1-b;*y=a;break;
 case METER_ROTATION_180:*x=width-1-a;*y=height-1-b;break;
 case METER_ROTATION_270:*x=b;*y=width-1-a;break;
 default:*x=a;*y=b;break;
 }
}
