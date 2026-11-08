#include "berry_button_policy.h"
berry_button_action_t berry_button_update(berry_button_policy_t *s,bool down,uint32_t now){
 if(!s)return BERRY_BUTTON_NONE;
 if(down && !s->down){s->down=true;s->down_at=now;s->long_fired=false;return BERRY_BUTTON_NONE;}
 if(down && s->down && !s->long_fired && (uint32_t)(now-s->down_at)>=800){s->long_fired=true;return BERRY_BUTTON_PANEL;}
 if(!down && s->down){s->down=false;if(s->long_fired){s->long_fired=false;return BERRY_BUTTON_NONE;}return BERRY_BUTTON_NEXT;}
 return BERRY_BUTTON_NONE;
}

berry_button_action_t berry_boot_button_update(berry_boot_button_t *s,bool down,uint32_t now){
 if(!s)return BERRY_BUTTON_NONE;
 if(!down && !s->down){s->armed=true;return BERRY_BUTTON_NONE;}
 if(down && !s->down){s->down=true;s->down_at=now;return BERRY_BUTTON_NONE;}
 if(!down && s->down){s->down=false;bool fire=s->armed && (uint32_t)(now-s->down_at)<800;s->armed=true;return fire?BERRY_BUTTON_PREVIOUS:BERRY_BUTTON_NONE;}
 return BERRY_BUTTON_NONE;
}
