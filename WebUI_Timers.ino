//======================================================
//   WEB UI: تنظیمات Timer / Scheduler / Blinker هر رله
//======================================================

// فعلاً فقط رله‌ی شماره 1 دکمه‌ی تنظیمات (⚙) داره.
// بعد از تایید، این مقدار رو بکن  TOUCH_COUNT  تا برای همه‌ی رله‌ها فعال بشه.
#define UI_SETTINGS_RELAYS TOUCH_COUNT

//------------------------------------------------------
//  بخش ثابت اول صفحه (HTML head + CSS)  —  در Flash
//------------------------------------------------------
const char PAGE_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP8266 Test</title>
<style>
body{font-family:Arial;text-align:center;background:#f0f0f0;margin:0;padding:20px;}
.relay{display:inline-block;white-space:nowrap;}
.btn{width:220px;height:60px;font-size:22px;border:none;border-radius:10px;color:white;margin:10px;cursor:pointer;}
.on{background:#2ecc71;}
.off{background:#e74c3c;}
.gear{width:50px;height:60px;font-size:22px;border:none;border-radius:10px;background:#34495e;color:#fff;margin:10px 10px 10px -4px;cursor:pointer;}
.panel{display:none;direction:rtl;text-align:right;background:#fff;border-radius:10px;padding:12px 14px;margin:10px auto;max-width:700px;box-shadow:0 1px 5px #0003;}
.panel h3{margin:0 0 6px;}
.note{font-size:12px;color:#666;line-height:1.7;margin-bottom:6px;}
.slot{border:1px solid #ddd;border-radius:8px;padding:8px 10px;margin:8px 0;}
.hd{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap;}
.f{display:flex;flex-wrap:wrap;gap:6px 14px;align-items:center;margin:7px 0;}
.f label{font-size:14px;}
.wd{border:1px solid #ccc;border-radius:6px;padding:2px 8px;}
input,select{font-size:14px;padding:3px 4px;margin:0 3px;}
input[type=number]{width:80px;}
.sv,.cl{color:#fff;border:0;border-radius:6px;padding:6px 16px;cursor:pointer;font-size:14px;}
.sv{background:#2980b9;}
.cl{background:#7f8c8d;}
.run{color:#27ae60;font-size:12px;}
.msg{font-size:13px;}
.tbox{display:inline-block;background:#fff;border-radius:10px;padding:8px 18px;margin:4px 0 10px;box-shadow:0 1px 5px #0003;font-size:15px;line-height:1.9;}
.tbox b{display:inline-block;min-width:150px;text-align:right;}
.tbox span{font-family:monospace;font-size:16px;}
</style></head><body>
)rawliteral";

//------------------------------------------------------
//  بخش ثابت آخر صفحه (JavaScript)  —  در Flash
//------------------------------------------------------
const char PAGE_TAIL[] PROGMEM = R"rawliteral(<script>
const DAYS=[['ش',6],['ی',0],['د',1],['س',2],['چ',3],['پ',4],['ج',5]]; // [نام, بیت weekMask]  (بیت0=یکشنبه)
const OFF=new Date().getTimezoneOffset();   // دقیقه: UTC - local
function $(i){return document.getElementById(i);}
function pad(n){return(n<10?'0':'')+n;}
function rot(m,s){let o=0;for(let d=0;d<7;d++)if(m&(1<<d))o|=1<<((((d+s)%7)+7)%7);return o;}
function conv(h,m,s,dir){let t=h*60+m+dir*OFF,ds=Math.floor(t/1440);t-=ds*1440;return{h:Math.floor(t/60),m:t%60,s:s,ds:ds};} // dir:+1 محلی→UTC ، -1 UTC→محلی

// ---- تبدیل ساعت/دقیقه/ثانیه <-> ثانیه (برای مدت‌ها: برگشت تایمر، روشن/خاموش چشمک‌زن) ----
function hms(pfx){return(+($(pfx+'h').value||0))*3600+(+($(pfx+'m').value||0))*60+(+($(pfx+'s').value||0));}
function fillHms(pfx,sec){sec=sec||0;$(pfx+'h').value=Math.floor(sec/3600);$(pfx+'m').value=Math.floor((sec%3600)/60);$(pfx+'s').value=sec%60;}

function toggleRelay(c){fetch('/toggle?ch='+c).then(update);}
function update(){
 let t=Math.floor(Date.now()/1000);
 fetch('/status?utc='+t).then(r=>r.json()).then(d=>{
  let b=document.getElementsByClassName('btn');
  for(let i=0;i<NR;i++){
   if(d.relay[i]==1){b[i].className='btn on';b[i].innerHTML='Relay '+(i+1)+' ON';}
   else{b[i].className='btn off';b[i].innerHTML='Relay '+(i+1)+' OFF';}
  }
 }).catch(()=>{});
}
setInterval(update,300);
window.onload=update;

// ---- ساعت خود دستگاه (از ماژول خوانده می‌شود، نه از مرورگر) ----
const QN=['UNAVAILABLE','ESTIMATED','SYNCED'],SN=['NONE','Client','NTP'],QC=['#c0392b','#e67e22','#27ae60'];
function fmt(d){return d.getUTCFullYear()+'-'+pad(d.getUTCMonth()+1)+'-'+pad(d.getUTCDate())+' '+pad(d.getUTCHours())+':'+pad(d.getUTCMinutes())+':'+pad(d.getUTCSeconds());}
(function(){const o=-OFF,a=Math.abs(o);$('tlh').textContent='Device Time (UTC'+(o<0?'-':'+')+pad(Math.floor(a/60))+':'+pad(a%60)+') :';})();
function devtime(){
 fetch('/devtime').then(r=>r.json()).then(d=>{
  const ok=d.quality>0;
  $('tu').textContent=ok?fmt(new Date(d.epoch*1000)):'--';
  $('tl').textContent=ok?fmt(new Date((d.epoch-OFF*60)*1000)):'--';
  const q=$('tq');q.textContent=QN[d.quality]+' ('+SN[d.source]+')';q.style.color=QC[d.quality];
 }).catch(()=>{const q=$('tq');q.textContent='no response';q.style.color='#c0392b';});
}
setInterval(devtime,1000);
devtime();

function msg(i,t,ok){const e=$('ms'+i);e.style.color=ok?'#27ae60':'#c0392b';e.textContent=t;}

function slotHTML(r,k){
 const i=r+'_'+k;
 let wd='';
 DAYS.forEach(x=>{wd+=`<label class='wd'><input type='checkbox' id='wd${i}_${x[1]}'>${x[0]}</label>`;});
 return `<div class='slot'>
 <div class='hd'><b>اسلات ${k+1}</b><span class='run' id='st${i}'></span></div>
 <div class='f'>
  <label>حالت <select id='md${i}' onchange='sync("${i}")'><option value='0'>زمان‌بند / تایمر</option><option value='1'>چشمک زن</option></select></label>
  <label>تکرار <select id='ty${i}' onchange='sync("${i}")'><option value='0'>یک‌بار</option><option value='1'>روزانه</option><option value='2'>هفتگی</option></select></label>
 </div>
 <div class='f'><label id='dtl${i}'>تاریخ <input type='date' id='da${i}'></label><label>ساعت <input type='time' step='1' id='tm${i}'></label></div>
 <div class='f' id='wk${i}'>${wd}</div>
 <div class='f' id='nm${i}'>
  <label>عملکرد <select id='ac${i}'><option value='1'>روشن شود</option><option value='0'>خاموش شود</option></select></label>
 </div>
 <div class='f' id='nm${i}b'>
  <label>برگشت به حالت قبل بعد از
   <input type='number' id='du${i}h' min='0' max='999'>ساعت
   <input type='number' id='du${i}m' min='0' max='59'>دقیقه
   <input type='number' id='du${i}s' min='0' max='59'>ثانیه
  </label> (همه صفر = بدون برگشت)
 </div>
 <div class='f' id='bl${i}'>
  <label>روشن
   <input type='number' id='bn${i}h' min='0' max='999'>ساعت
   <input type='number' id='bn${i}m' min='0' max='59'>دقیقه
   <input type='number' id='bn${i}s' min='0' max='59'>ثانیه
  </label>
 </div>
 <div class='f' id='bl${i}b'>
  <label>خاموش
   <input type='number' id='bf${i}h' min='0' max='999'>ساعت
   <input type='number' id='bf${i}m' min='0' max='59'>دقیقه
   <input type='number' id='bf${i}s' min='0' max='59'>ثانیه
  </label>
  <label>تعداد <input type='number' id='bc${i}' min='1' max='65535'> بار</label>
 </div>
 <div class='f'><button class='sv' onclick='save(${r},${k})'>ذخیره</button><button class='cl' onclick='clr(${r},${k})'>حذف</button><span class='msg' id='ms${i}'></span></div>
 </div>`;
}

function sync(i){
 const md=+$('md'+i).value,ty=+$('ty'+i).value;
 $('dtl'+i).style.display=ty==0?'':'none';
 $('wk'+i).style.display=ty==2?'':'none';
 $('nm'+i).style.display=md==0?'':'none';
 $('nm'+i+'b').style.display=md==0?'':'none';
 $('bl'+i).style.display=md==1?'':'none';
 $('bl'+i+'b').style.display=md==1?'':'none';
}

function fill(r,k,t){
 const i=r+'_'+k;
 $('md'+i).value=t.mode;
 $('ty'+i).value=t.type;
 const L=conv(t.hour,t.minute,t.second,-1);
 $('tm'+i).value=pad(L.h)+':'+pad(L.m)+':'+pad(L.s);
 let d=new Date();
 if(t.type==0&&t.month>=1)d=new Date(Date.UTC(2000+t.year,t.month-1,t.day,t.hour,t.minute,t.second));
 $('da'+i).value=d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate());
 const wm=rot(t.weekMask,L.ds);
 DAYS.forEach(x=>{$('wd'+i+'_'+x[1]).checked=!!(wm&(1<<x[1]));});
 $('ac'+i).value=t.state;
 fillHms('du'+i,t.duration);
 fillHms('bn'+i,t.blinkOn||BMIN);
 fillHms('bf'+i,t.blinkOff||BMIN);
 $('bc'+i).value=t.blinkCount||1;
 let s=t.enabled?'':'— خالی —';
 if(t.enabled&&t.running)s+=' ▶ در حال اجرا';
 if(t.enabled&&t.relay!=r)s+=' ⚠ این اسلات برای رله '+(t.relay+1)+' تنظیم شده؛ با ذخیره به این رله منتقل می‌شود';
 $('st'+i).textContent=s;
 sync(i);
}

function load(r){
 fetch('/getTimers?relay='+r).then(x=>x.json()).then(a=>{a.forEach(t=>fill(r,t.slot-r*SLOTS,t));}).catch(()=>{});
}

function send(q,r,i,ok){
 fetch('/setTimer?'+q).then(x=>x.text().then(t=>{
  if(x.ok){msg(i,ok,1);load(r);}else msg(i,t,0);
 })).catch(()=>msg(i,'خطا در ارتباط با دستگاه',0));
}

function save(r,k){
 const i=r+'_'+k,md=+$('md'+i).value,ty=+$('ty'+i).value;
 const tm=$('tm'+i).value.split(':');
 if(tm.length<2){msg(i,'ساعت را وارد کنید',0);return;}
 const h=+tm[0],mi=+tm[1],s=+(tm[2]||0);
 let Y=0,M=1,D=1,H,MI,S,wm=0;
 if(ty==0){
  const dv=$('da'+i).value;
  if(!dv){msg(i,'تاریخ را وارد کنید',0);return;}
  const p=dv.split('-'),d=new Date(+p[0],+p[1]-1,+p[2],h,mi,s);
  Y=d.getUTCFullYear()-2000;M=d.getUTCMonth()+1;D=d.getUTCDate();
  H=d.getUTCHours();MI=d.getUTCMinutes();S=d.getUTCSeconds();
  if(Y<0||Y>99){msg(i,'سال باید بین 2000 تا 2099 باشد',0);return;}
 }else{
  const u=conv(h,mi,s,1);H=u.h;MI=u.m;S=u.s;
  if(ty==2){
   let m=0;DAYS.forEach(x=>{if($('wd'+i+'_'+x[1]).checked)m|=1<<x[1];});
   if(!m){msg(i,'حداقل یک روز هفته را انتخاب کنید',0);return;}
   wm=rot(m,u.ds);
  }
 }
 // با ذخیره، این اسلات همیشه فعال می‌شود؛ برای غیرفعال‌کردن از دکمه‌ی «حذف» استفاده کن
 let q='slot='+(r*SLOTS+k)+'&enabled=1&type='+ty+'&year='+Y+'&month='+M+'&day='+D+
  '&hour='+H+'&minute='+MI+'&second='+S+'&weekMask='+wm+'&relay='+r+'&mode='+md;
 if(md==0){
  const du=hms('du'+i);
  q+='&state='+$('ac'+i).value+'&duration='+du;
 }else{
  const bn=hms('bn'+i),bf=hms('bf'+i),bc=+$('bc'+i).value;
  if(bn<BMIN||bn>BMAX||bf<BMIN||bf>BMAX){msg(i,'زمان روشن/خاموش باید بین '+BMIN+' تا '+BMAX+' ثانیه باشد',0);return;}
  if(bc<1||bc>65535){msg(i,'تعداد باید حداقل 1 باشد',0);return;}
  q+='&state=1&blinkOn='+bn+'&blinkOff='+bf+'&blinkerCount='+bc;
 }
 send(q,r,i,'✓ ذخیره شد');
}

function clr(r,k){
 send('slot='+(r*SLOTS+k)+'&enabled=0&type=1&year=0&month=1&day=1&hour=0&minute=0&second=0&weekMask=0&relay='+r+'&mode=0&state=0&duration=0',
  r,r+'_'+k,'✓ حذف شد');
}

// ---- بازیابی رله بعد از بوت (سطح رله، نه اسلات) ----
function loadRestore(r){
 fetch('/getRelayRestore').then(x=>x.json()).then(d=>{$('rr'+r).checked=!!d.restore[r];}).catch(()=>{});
}
function saveRestore(r){
 const v=$('rr'+r).checked?1:0;
 fetch('/setRelayRestore?relay='+r+'&restore='+v).then(x=>x.text()).then(()=>{
  const e=$('rrm'+r);e.style.color='#27ae60';e.textContent='✓ ذخیره شد';
 }).catch(()=>{const e=$('rrm'+r);e.style.color='#c0392b';e.textContent='خطا در ارتباط با دستگاه';});
}

function openPanel(r){
 let p=$('pn'+r);
 if(!p){
  p=document.createElement('div');p.id='pn'+r;p.className='panel';
  let h=`<h3>تنظیمات رله ${r+1}</h3>
  <div class='f'><label><input type='checkbox' id='rr${r}'> بعد از ریست دستگاه، وضعیت و تایمرهای این رله بازیابی شوند</label>
  <button class='sv' onclick='saveRestore(${r})'>ذخیره</button><span class='msg' id='rrm${r}'></span></div>`;
  for(let k=0;k<SLOTS;k++)h+=slotHTML(r,k);
  h+=`<div class='note'>ساعت و تاریخ به وقت محلی مرورگر شما هستند (به‌صورت خودکار به UTC تبدیل می‌شوند).<br>
  زمان‌بند/تایمر: در زمان تعیین‌شده رله روشن/خاموش می‌شود؛ اگر «برگشت» صفر نباشد، پس از آن مدت به حالت مخالف برمی‌گردد.<br>
  چشمک زن: با روشن‌شدن شروع می‌شود، به تعداد دفعات تعیین‌شده (روشن+خاموش) تکرار می‌شود و در پایان رله خاموش می‌شود.<br>
  اجرای خودکار فقط وقتی فعال است که ساعت دستگاه Sync شده باشد.<br>
  اگر «بازیابی بعد از ریست» برای این رله خاموش باشد، این رله صرف‌نظر از وضعیت قبلی یا تایمرهای فعال، همیشه بعد از روشن‌شدن مجدد دستگاه خاموش می‌ماند.</div>`;
  p.innerHTML=h;$('panels').appendChild(p);
 }
 const show=p.style.display!='block';
 document.querySelectorAll('.panel').forEach(x=>x.style.display='none');
 if(show){p.style.display='block';load(r);loadRestore(r);}
}
</script></body></html>
)rawliteral";

//------------------------------------------------------
//  صفحه‌ی اصلی جدید  (server.on("/", handleRootV2))
//------------------------------------------------------
void handleRootV2()
{
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    server.sendContent_P(PAGE_HEAD);

    String s;
    s.reserve(900);

    s += F("<h2>Smart KEY Mahasa-elc Test</h2><p><b>IP :</b> ");
    s += WiFi.localIP().toString();
    s += F("</p><p><b>RSSI :</b> ");
    s += (int)WiFi.RSSI();
    s += F(" dBm</p><p><b>Host :</b> ");
    s += hostName;                       // نام دستگاه برای mDNS
    s += F(".local</p>"
           "<div class='tbox'>"
           "<div><b>Device Time (UTC) :</b> <span id='tu'>--</span></div>"
           "<div><b id='tlh'>Device Time (Local) :</b> <span id='tl'>--</span></div>"
           "<div><b>Time Sync :</b> <span id='tq'>--</span></div>"
           "</div><br><div id='row'>");
    server.sendContent(s);

    for (int i = 0; i < TOUCH_COUNT; i++)
    {
        s = "";
        s += F("<div class='relay'><button class='btn ");
        s += relayState[i] ? "on" : "off";
        s += F("' onclick='toggleRelay(");
        s += i;
        s += F(")'>Relay ");
        s += (i + 1);
        s += relayState[i] ? " ON" : " OFF";
        s += F("</button>");

        if (i < UI_SETTINGS_RELAYS)
        {
            s += F("<button class='gear' title='Settings' onclick='openPanel(");
            s += i;
            s += F(")'>&#9881;</button>");
        }

        s += F("</div>");
        server.sendContent(s);
    }

    // ثابت‌های لازم برای جاوااسکریپت (از #define های فرمور)
    s = F("</div><div id='panels'></div><script>const NR=");
    s += TOUCH_COUNT;
    s += F(",SLOTS=");
    s += (MAX_TIMERS / TOUCH_COUNT);
    s += F(",BMIN=");
    s += (unsigned long)BLINK_MIN_SECONDS;
    s += F(",BMAX=");
    s += (unsigned long)BLINK_MAX_SECONDS;
    s += F(";</script>");
    server.sendContent(s);

    server.sendContent_P(PAGE_TAIL);

    server.sendContent("");              // پایان پاسخ (chunked)
}

//------------------------------------------------------
//  /devtime  →  ساعت خود ماژول (server.on("/devtime", handleDevTime))
//  epoch = همان time(nullptr) که scheduler با آن کار می‌کند (UTC)
//  quality: 0=UNAVAILABLE  1=ESTIMATED  2=SYNCED
//  source : 0=NONE  1=CLIENT  2=NTP
//------------------------------------------------------
void handleDevTime()
{
    String json;
    json.reserve(80);

    json += F("{\"epoch\":");
    json += (unsigned long)getCurrentTime();
    json += F(",\"quality\":");
    json += (int)timeManager.quality;
    json += F(",\"source\":");
    json += (int)timeManager.source;
    json += '}';

    server.send(200, "application/json", json);
}

//------------------------------------------------------
//  بازیابی رله بعد از بوت (سطح رله، نه اسلات تایمر)
//  server.on("/getRelayRestore", handleGetRelayRestore)
//  server.on("/setRelayRestore", handleSetRelayRestore)
//------------------------------------------------------
void handleGetRelayRestore()
{
    String json;
    json.reserve(40);
    json += F("{\"restore\":[");

    for (int i = 0; i < TOUCH_COUNT; i++)
    {
        if (i > 0)
            json += ',';
        json += relayRestoreOnBoot[i] ? '1' : '0';
    }

    json += F("]}");

    server.send(200, "application/json", json);
}

void handleSetRelayRestore()
{
    if (!server.hasArg("relay") || !server.hasArg("restore"))
    {
        server.send(400, "text/plain", "Missing relay/restore");
        return;
    }

    int r = server.arg("relay").toInt();

    if (r < 0 || r >= TOUCH_COUNT)
    {
        server.send(400, "text/plain", "Bad relay");
        return;
    }

    bool restore = server.arg("restore").toInt() != 0;
    setRelayRestoreOnBoot((byte)r, restore, true);

    server.send(200, "text/plain", "OK");
}

//------------------------------------------------------
//  /getTimers  نسخه‌ی کامل  (server.on("/getTimers", handleGetTimersV2))
//  /getTimers            → همه‌ی اسلات‌ها
//  /getTimers?relay=0    → فقط اسلات‌های رله‌ی 1
//  (کلیدهای قبلی حفظ شدن؛ فقط کلیدهای جدید اضافه شده)
//------------------------------------------------------
void handleGetTimersV2()
{
    uint8_t from = 0;
    uint8_t to   = MAX_TIMERS;

    if (server.hasArg("relay"))
    {
        int r = server.arg("relay").toInt();

        if (r < 0 || r >= TOUCH_COUNT)
        {
            server.send(400, "text/plain", "Bad relay");
            return;
        }

        uint8_t per = MAX_TIMERS / TOUCH_COUNT;
        from = r * per;
        to   = from + per;
    }

    String json;
    json.reserve((to - from) * 260 + 8);
    json += '[';

    for (uint8_t i = from; i < to; i++)
    {
        Timer &t = timers[i];

        bool running = (t.mode == TIMER_MODE_BLINKER)
                       ? (t.blinkStartTime != 0)
                       : (t.activeUntil != 0);

        if (i > from)
            json += ',';

        json += F("{\"slot\":");       json += (int)i;
        json += F(",\"enabled\":");    json += (t.enabled ? 1 : 0);
        json += F(",\"type\":");       json += (int)t.type;
        json += F(",\"year\":");       json += (int)t.year;
        json += F(",\"month\":");      json += (int)t.month;
        json += F(",\"day\":");        json += (int)t.day;
        json += F(",\"hour\":");       json += (int)t.hour;
        json += F(",\"minute\":");     json += (int)t.minute;
        json += F(",\"second\":");     json += (int)t.second;
        json += F(",\"weekMask\":");   json += (int)t.weekMask;
        json += F(",\"relay\":");      json += (int)t.relayIndex;
        json += F(",\"state\":");      json += (t.relayState ? 1 : 0);
        json += F(",\"duration\":");   json += (int)t.durationSeconds;
        json += F(",\"mode\":");       json += (int)t.mode;
        json += F(",\"blinkOn\":");    json += (unsigned long)t.blinkOnSeconds;
        json += F(",\"blinkOff\":");   json += (unsigned long)t.blinkOffSeconds;
        json += F(",\"blinkCount\":"); json += (int)t.blinkerCount;
        json += F(",\"running\":");    json += (running ? 1 : 0);
        json += '}';
    }

    json += ']';

    server.send(200, "application/json", json);
}
