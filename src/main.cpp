#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <lvgl.h>
#include "lgfx_cyd.h"
#include "config.h"
#include <time.h>
LGFX tft;
Preferences prefs;
constexpr int W=320,H=240,MAX_PROJECTS=40;
#include "display.inc"
struct Project { char name[72]; int rank,stage; };
struct Snapshot { Project rows[MAX_PROJECTS]; int count; char forecast[80]; char checked[40]; bool ok; };
static Snapshot current{}, incoming{};
static QueueHandle_t requests, results;
static lv_obj_t *summary,*status,*list,*forecast;
static uint32_t lastRequest=0;
static bool busy=false;
static constexpr int PER_PAGE=6;
static constexpr uint32_t PAGE_MS=10000;
static int page=0; static bool paused=false; static uint32_t pageAt=0;
static lv_obj_t *pageLabel,*wifiLabel,*countdown;
static bool progressed[MAX_PROJECTS]{};
static bool cached=false, networkReady=false, failed=false;
static const char* translated(const char* raw){
 static String out; out=raw; out.replace("about ","env. "); out.replace(" days"," jours"); out.replace(" day"," jour"); out.replace(" hours"," heures"); out.replace(" hour"," heure"); out.replace(" minutes"," min"); out.replace(" minute"," min"); out.replace(" weeks"," semaines"); out.replace(" week"," semaine"); return out.c_str();
}
static void sourceText(){
 if(!current.count)return;
 int y,m,d,h,n; char buf[32];
 if(sscanf(current.checked,"%d-%d-%dT%d:%d",&y,&m,&d,&h,&n)==5){
  struct tm utc{};utc.tm_year=y-1900;utc.tm_mon=m-1;utc.tm_mday=d;utc.tm_hour=h;utc.tm_min=n;utc.tm_isdst=0;
  setenv("TZ","UTC0",1);tzset();time_t stamp=mktime(&utc);
  setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1);tzset();struct tm local{};
  localtime_r(&stamp,&local);strftime(buf,sizeof(buf),"%d/%m %H:%M",&local);
  lv_label_set_text_fmt(status,"%s: %s",cached?"Cache":failed?"Echec - source":"Source",buf);
 }else lv_label_set_text_fmt(status,"%s: %.16s",cached?"Cache":"Source",current.checked);
}
static void loadCache(){
 if(!prefs.begin("gta6",true))return;
 if(prefs.getBytesLength("snapshot1")==sizeof(current)){
  prefs.getBytes("snapshot1",&current,sizeof(current));
  if(!current.ok||current.count<1||current.count>MAX_PROJECTS)current=Snapshot{};
  else {cached=true;current.checked[sizeof(current.checked)-1]=0;current.forecast[sizeof(current.forecast)-1]=0;for(int i=0;i<current.count;i++){current.rows[i].name[71]=0;if(current.rows[i].stage<1||current.rows[i].stage>7){current=Snapshot{};cached=false;break;}}}
 }prefs.end();
}
static void saveCache(){if(prefs.begin("gta6",false)){prefs.putBytes("snapshot1",&current,sizeof(current));prefs.end();}}

static lv_obj_t *rows[PER_PAGE], *ranks[PER_PAGE], *names[PER_PAGE], *levels[PER_PAGE], *segments[PER_PAGE][7];
static String clean(String s){ String out; bool tag=false; for(unsigned i=0;i<s.length();i++){char c=s[i];if(c=='<')tag=true;else if(c=='>')tag=false;else if(!tag)out+=c;} out.replace("&amp;","&");out.replace("&nbsp;"," ");out.replace("&#x27;","'");out.trim();return out; }
static String between(const String& s,const char* start,const char* end){int a=s.indexOf(start);if(a<0)return "";a+=strlen(start);int b=s.indexOf(end,a);return b<0?String():s.substring(a,b);}
static bool fetch(Snapshot &o){
 o=Snapshot{}; if(WiFi.status()!=WL_CONNECTED)return false;
 WiFiClientSecure client; client.setInsecure(); // Public data, no certificate validation in V1
 HTTPClient http;http.useHTTP10(true);http.setConnectTimeout(8000);http.setTimeout(10000);
 if(!http.begin(client,TRACKER_URL))return false;
 int code=http.GET();Serial.printf("HTTP %d\n",code);if(code!=200){http.end();return false;}
 auto &stream=http.getStream(); String line,row;line.reserve(4096);row.reserve(4096);bool inRow=false,done=false,overflow=false;uint32_t last=millis(),start=last;
 int remaining=http.getSize();
 while((remaining!=0)&&(stream.connected()||stream.available())&&millis()-last<10000&&millis()-start<45000){
  if(!stream.available()){delay(2);continue;}int c=stream.read();if(c<0)continue;if(remaining>0)--remaining;last=millis();
  if(c!='\n'&&remaining!=0){if(line.length()<20000)line+=(char)c;else overflow=true;continue;} if(c!='\n'&&line.length()<20000)line+=(char)c;
  if(!overflow){
   if(line.indexOf("id=\"checked\"")>=0){String v=between(line,"id=\"checked\" datetime=\"","\"");strlcpy(o.checked,v.c_str(),sizeof(o.checked));}
   if(line.indexOf("id=\"fc-short\"")>=0){int a=line.indexOf("id=\"fc-short\"");String v=clean(between(line.substring(a),">","</span>"));strlcpy(o.forecast,v.c_str(),sizeof(o.forecast));}
   if(line.indexOf("<li class=\"board__row\"")>=0){inRow=true;row="";}
   if(inRow){if(row.length()+line.length()>12000){inRow=false;http.end();return false;}row+=line;if(line.indexOf("</li>")>=0){
    String rank=between(row,"aria-label=\"Position ","\"");String name=clean(between(row,"<h3 class=\"t-card\">","</h3>"));String stage=between(row,"aria-label=\"Stage "," of 7");int n=stage.toInt();
    if(!rank.length()||!name.length()||n<1||n>7||o.count>=MAX_PROJECTS){http.end();return false;}auto &p=o.rows[o.count++];p.rank=rank.toInt();p.stage=n;strlcpy(p.name,name.c_str(),sizeof(p.name));inRow=false;}}
   if(o.count&&!inRow&&line.indexOf("</ol>")>=0){done=true;break;}
  }else if(inRow){http.end();return false;}line="";overflow=false;
 }
 http.end();return done&&o.count>0;
}
static void network(void*){uint8_t cmd;for(;;){if(xQueueReceive(requests,&cmd,portMAX_DELAY)){incoming.ok=fetch(incoming);xQueueSend(results,&incoming,portMAX_DELAY);}}}
static lv_obj_t* label(lv_obj_t* p,const char* t,int x,int y,const lv_font_t* f,uint32_t color){auto l=lv_label_create(p);lv_label_set_text(l,t);lv_obj_set_pos(l,x,y);lv_obj_set_style_text_font(l,f,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);return l;}
static void refresh(){if(busy||!networkReady||WiFi.status()!=WL_CONNECTED)return;uint8_t cmd=1;if(xQueueSend(requests,&cmd,0)==pdTRUE){busy=true;lastRequest=millis();lv_label_set_text(status,"Actualisation...");}}
static void click(lv_event_t*){refresh();}
static int pages(){return max(1,(current.count+PER_PAGE-1)/PER_PAGE);}
static void render(){
 page=constrain(page,0,pages()-1); int best=0;
 for(int i=0;i<current.count;i++)best=max(best,current.rows[i].stage);
 for(int i=0;i<PER_PAGE;i++){
  int k=page*PER_PAGE+i;
  if(k>=current.count){lv_obj_add_flag(rows[i],LV_OBJ_FLAG_HIDDEN);continue;}
  lv_obj_remove_flag(rows[i],LV_OBJ_FLAG_HIDDEN); auto &p=current.rows[k];
  lv_label_set_text_fmt(ranks[i],"%02d",p.rank);lv_label_set_text(names[i],p.name);lv_obj_set_style_text_color(names[i],lv_color_hex(progressed[k]?0x55dd88:0xffffff),0);
  lv_label_set_text_fmt(levels[i],"%d/7",p.stage);
  for(int j=0;j<7;j++)lv_obj_set_style_bg_color(segments[i][j],lv_color_hex(j<p.stage?0xf49ccd:0x414354),0);
 }
 if(current.count){lv_label_set_text_fmt(summary,"%d projets | %d/7 | %s",current.count,best,best==7?"Jouable":"Non jouable");lv_label_set_text_fmt(forecast,"Est. site : %s",current.forecast[0]?translated(current.forecast):"--");}
 lv_label_set_text_fmt(pageLabel,"%s %d/%d",paused?"Pause":"Page",page+1,pages());
}
static void navigate(lv_event_t* e){
 auto code=lv_event_get_code(e); if(code!=LV_EVENT_CLICKED)return;
 lv_point_t p; auto in=lv_indev_active(); if(!in)return;lv_indev_get_point(in,&p);
 if(p.x<107)page=(page+pages()-1)%pages();else if(p.x>213)page=(page+1)%pages();else paused=!paused;
 pageAt=millis();render();
}
static void buildRows(){
 for(int i=0;i<PER_PAGE;i++){
  auto r=rows[i]=lv_obj_create(list);lv_obj_set_pos(r,0,i*21);lv_obj_set_size(r,300,20);
  lv_obj_set_style_pad_all(r,0,0);lv_obj_set_style_border_width(r,0,0);lv_obj_set_style_radius(r,4,0);lv_obj_set_style_bg_color(r,lv_color_hex(i%2?0x191b28:0x242637),0);
  lv_obj_remove_flag(r,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
  ranks[i]=label(r,"",5,3,&lv_font_montserrat_12,0xa0a5bd);
  names[i]=label(r,"",29,3,&lv_font_montserrat_12,0xffffff);lv_obj_set_width(names[i],145);lv_label_set_long_mode(names[i],LV_LABEL_LONG_DOT);
  for(int j=0;j<7;j++){auto seg=segments[i][j]=lv_obj_create(r);lv_obj_set_pos(seg,178+j*11,6);lv_obj_set_size(seg,8,8);lv_obj_set_style_border_width(seg,0,0);lv_obj_set_style_radius(seg,1,0);lv_obj_remove_flag(seg,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));}
  levels[i]=label(r,"",258,3,&lv_font_montserrat_12,0xffffff);lv_obj_add_flag(r,LV_OBJ_FLAG_HIDDEN);
 }
}

// Boot animation: direct RGB565 drawing, no extra framebuffer.
static bool bootActive=true, bootResult=false;
static uint32_t bootAt=0, bootFrame=0;
static int bootPhase=-1;
static uint16_t bootColor(int r,int g,int b){return tft.color565(r,g,b);}
static void palm(int x,int y,int lean){
 uint16_t c=bootColor(25,13,45);int tx=x+lean;
 tft.fillTriangle(x-4,166,x+5,166,tx,y,c);
 tft.fillTriangle(x+5,166,tx+3,y,tx,y,c);
 for(int k=0;k<5;k++){
  int dx[]={-39,-30,0,29,39};int dy[]={12,-13,-25,-13,12};
  tft.fillTriangle(tx,y,tx+dx[k],y+dy[k],tx+dx[k]*2/3,y+dy[k]+10,c);
 }
}
static void bootStart(){
 bootAt=millis();bootFrame=0;bootPhase=-1;
 for(int y=0;y<166;y++){
  int r=38+210*y/165,g=19+83*y/165,b=100+20*y/165;
  tft.drawFastHLine(0,y,W,bootColor(r,g,b));
 }
 tft.fillCircle(160,103,48,bootColor(255,178,105));
 for(int y=113;y<151;y+=8)tft.drawFastHLine(112,y,96,bootColor(224,91,137));
 palm(24,66,12);palm(296,76,-12);
 tft.fillRect(0,166,W,74,bootColor(16,18,28));
 tft.setTextDatum(textdatum_t::middle_center);
 tft.setTextSize(1);tft.setTextColor(bootColor(255,190,220));
 tft.drawString("PC RACE",160,144);
}
static void bootDraw(bool online){
 uint32_t elapsed=millis()-bootAt;
 if(elapsed<bootFrame+80)return;bootFrame=elapsed;
 int phase=elapsed<600?0:!networkReady?4:bootResult?(failed?4:3):online?2:1;
 if(phase!=bootPhase){
  bootPhase=phase;
  if(phase!=0){tft.setTextSize(4);tft.setTextColor(bootColor(40,17,60));tft.drawString("GTA VI",162,91);tft.setTextColor(bootColor(255,220,238));tft.drawString("GTA VI",160,89);}
  tft.fillRect(0,172,W,22,bootColor(16,18,28));tft.setTextSize(1);tft.setTextColor(bootColor(255,190,220));
  const char* msg=phase==0?"INITIALISATION":phase==1?"CONNEXION WI-FI":phase==2?"RECUPERATION DU CLASSEMENT":phase==3?"DASHBOARD PRET":current.count?"CLASSEMENT EN CACHE":"MODE HORS LIGNE";
  tft.drawString(msg,160,183);
 }
 int progress=phase==0?10:phase==1?30:phase==2?65:100;
 tft.fillRoundRect(24,200,272,10,5,bootColor(48,43,70));
 int width=progress*268/100;
 for(int x=0;x<width;x++){int glow=(x+elapsed/8)%90;int g=65+(glow<30?glow*3:0);tft.drawFastVLine(26+x,202,6,bootColor(245,g,155-x*65/268));}
 tft.fillRect(0,218,W,18,bootColor(16,18,28));tft.setTextSize(1);tft.setTextColor(bootColor(190,165,210));
 tft.drawString("WELCOME TO LEONIDA",160,226);
 // Never block startup indefinitely: dashboard keeps fetching in background.
 if((elapsed>=2600&&(bootResult||!networkReady))||elapsed>=12000){bootActive=false;pageAt=millis();lv_obj_invalidate(lv_screen_active());}
}

void setup(){Serial.begin(115200);pinMode(0,INPUT_PULLUP);tft.init();tft.setRotation(3);tft.setBrightness(220);prefs.begin("gta6",false);uint16_t cal[8];if(prefs.getBytesLength("cal")==sizeof(cal)&&digitalRead(0)!=LOW){prefs.getBytes("cal",cal,sizeof(cal));tft.setTouchCalibrate(cal);}else{tft.fillScreen(TFT_BLACK);tft.calibrateTouch(cal,TFT_MAGENTA,TFT_BLACK,15);prefs.putBytes("cal",cal,sizeof(cal));tft.setTouchCalibrate(cal);}prefs.end();
 bootStart();
 lv_init();lv_tick_set_cb(tick_cb);auto d=lv_display_create(W,H);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_flush_cb(d,flush_cb);lv_display_set_buffers(d,drawBuf,nullptr,sizeof(drawBuf),LV_DISPLAY_RENDER_MODE_PARTIAL);auto in=lv_indev_create();lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(in,touch_cb);
 auto root=lv_screen_active();lv_obj_set_style_bg_color(root,lv_color_hex(0x10121c),0);lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);label(root,"GTA VI | PC RACE",10,3,&lv_font_montserrat_24,0xf49ccd);summary=label(root,"En attente des donnees",10,32,&lv_font_montserrat_12,0xffb875);forecast=label(root,"Est. site : --",10,53,&lv_font_montserrat_10,0xffffff);lv_obj_set_width(forecast,300);lv_label_set_long_mode(forecast,LV_LABEL_LONG_DOT);list=lv_obj_create(root);lv_obj_set_pos(list,8,72);lv_obj_set_size(list,304,126);lv_obj_set_style_bg_opa(list,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(list,0,0);lv_obj_set_style_pad_all(list,0,0);lv_obj_remove_flag(list,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(list,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(list,navigate,LV_EVENT_CLICKED,nullptr);buildRows();pageLabel=label(root,"Page 1/1",8,201,&lv_font_montserrat_12,0xf49ccd);status=label(root,"Connexion Wi-Fi...",8,218,&lv_font_montserrat_10,0xb5b8cc);lv_obj_set_width(status,220);lv_label_set_long_mode(status,LV_LABEL_LONG_DOT);auto btn=lv_button_create(root);lv_obj_set_pos(btn,233,207);lv_obj_set_size(btn,80,30);lv_obj_set_style_bg_color(btn,lv_color_hex(0x713c65),0);auto txt=lv_label_create(btn);lv_label_set_text(txt,"Refresh");lv_obj_set_style_text_font(txt,&lv_font_montserrat_12,0);lv_obj_center(txt);lv_obj_add_event_cb(btn,click,LV_EVENT_CLICKED,nullptr);
 wifiLabel=label(root,"WiFi",268,35,&lv_font_montserrat_10,0xff6666);countdown=label(root,"--:--",135,202,&lv_font_montserrat_10,0xb5b8cc);loadCache();if(current.count){render();sourceText();}requests=xQueueCreate(1,sizeof(uint8_t));results=xQueueCreate(1,sizeof(Snapshot));if(!requests||!results||xTaskCreate(network,"tracker",12288,nullptr,1,nullptr)!=pdPASS){lv_label_set_text(status,"Erreur allocation reseau");return;}networkReady=true;WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.begin(WIFI_SSID,WIFI_PASS);
}
void loop(){
 if(!bootActive)lv_timer_handler();static bool wasOnline=false;bool online=WiFi.status()==WL_CONNECTED;
 if(networkReady){
  if(online&&!wasOnline)refresh();
  if(online&&!busy&&millis()-lastRequest>=REFRESH_MS)refresh();
  static Snapshot received;
  if(xQueueReceive(results,&received,0)==pdTRUE){
   busy=false;failed=!received.ok;bootResult=true;
   if(received.ok){
    memset(progressed,0,sizeof(progressed));
    for(int i=0;i<received.count;i++)for(int j=0;j<current.count;j++)if(strcmp(received.rows[i].name,current.rows[j].name)==0){progressed[i]=received.rows[i].stage>current.rows[j].stage;break;}
    bool changed=memcmp(&current,&received,sizeof(current))!=0;
    current=received;cached=false;pageAt=millis();render();sourceText();if(changed)saveCache();
   }else if(current.count)sourceText();else lv_label_set_text(status,"Erreur reseau / HTML - Refresh");
  }
 }
 if(!online&&wasOnline&&!busy){if(current.count)sourceText();else lv_label_set_text(status,"Wi-Fi deconnecte");}
 wasOnline=online;
 static uint32_t uiAt=0;static bool first=true;
 if(first||millis()-uiAt>=500){first=false;uiAt=millis();lv_obj_set_style_text_color(wifiLabel,lv_color_hex(online?0x55dd88:0xff6666),0);
  if(busy)lv_label_set_text(countdown,"MAJ...");else if(!online)lv_label_set_text(countdown,"Hors ligne");else{uint32_t elapsed=millis()-lastRequest;uint32_t sec=elapsed>=REFRESH_MS?0:(REFRESH_MS-elapsed+999)/1000;lv_label_set_text_fmt(countdown,"MAJ %02u:%02u",(unsigned)(sec/60),(unsigned)(sec%60));}}
 if(bootActive)bootDraw(online);
 if(!bootActive&&!paused&&current.count&&millis()-pageAt>=PAGE_MS){page=(page+1)%pages();pageAt=millis();render();}delay(5);
}
