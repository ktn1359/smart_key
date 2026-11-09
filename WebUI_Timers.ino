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
.time-wheel{min-width:76px;height:38px;font-size:18px;font-weight:bold;text-align:center;
 border:1px solid #aaa;border-radius:7px;background:#fff;direction:ltr;}
.all-days{font-weight:bold;background:#eef6ff;border-color:#8bb9e8;}
.sv,.cl{color:#fff;border:0;border-radius:6px;padding:6px 16px;cursor:pointer;font-size:14px;}
.sv{background:#2980b9;}
.cl{background:#7f8c8d;}
.run{color:#27ae60;font-size:12px;}
.wire{font-family:monospace;font-size:11px;color:#52616b;direction:ltr;text-align:left;line-height:1.6;overflow-wrap:anywhere;}
.msg{font-size:13px;}
.tbox{display:inline-block;background:#fff;border-radius:10px;padding:8px 18px;margin:4px 0 10px;box-shadow:0 1px 5px #0003;font-size:15px;line-height:1.9;}
.tbox b{display:inline-block;min-width:150px;text-align:right;}
.tbox span{font-family:monospace;font-size:16px;}
.lnk{display:inline-block;background:#8e44ad;color:#fff;text-decoration:none;border-radius:8px;padding:8px 18px;font-size:16px;}
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
let clientTimeSent=false;
function update(){
 if(!clientTimeSent){clientTimeSent=true;fetch('/timesync?utc='+Math.floor(Date.now()/1000)).catch(()=>{});}
 fetch('/status').then(r=>r.json()).then(d=>{
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
 DAYS.forEach(x=>{wd+=`<label class='wd'><input type='checkbox' id='wd${i}_${x[1]}' onchange='dayChanged("${i}")'>${x[0]}</label>`;});
 let hourOptions='',minuteOptions='';
 for(let n=0;n<24;n++)hourOptions+=`<option value='${n}'>${pad(n)}</option>`;
 for(let n=0;n<60;n++)minuteOptions+=`<option value='${n}'>${pad(n)}</option>`;
 return `<div class='slot'>
 <div class='hd'><b>تایمر ${k+1} (از ۲ تایمر این رله)</b><span class='run' id='st${i}'></span></div>
 <div class='f'><b>روزهای هفته:</b>
  <label class='wd all-days'><input type='checkbox' id='wa${i}' onchange='toggleAllDays("${i}")'> تمامی روزها</label>${wd}
 </div>
 <div class='f'><label>ساعت شروع
  <select class='time-wheel' id='sh${i}'>${hourOptions}</select>
  :
  <select class='time-wheel' id='sm${i}'>${minuteOptions}</select>
  <span class='note'>برای انتخاب ساعت و دقیقه، فهرست را باز کنید و مقدار موردنظر را انتخاب کنید.</span>
 </label></div>
 <div class='f'>
  <label>مدت روشن بودن
   <input type='number' id='on${i}h' min='0' max='999'>ساعت
   <input type='number' id='on${i}m' min='0' max='59'>دقیقه
   <input type='number' id='on${i}s' min='0' max='59'>ثانیه
  </label>
 </div>
 <div class='f'>
  <label><input type='checkbox' id='be${i}' onchange='sync("${i}")'> با فعال کردن این گزینه، تایمر به حالت چشمک‌زن تبدیل می‌شود.</label>
 </div>
 <div class='f' id='offrow${i}'>
  <label>مدت خاموش بودن
   <input type='number' id='off${i}h' min='0' max='999' disabled>ساعت
   <input type='number' id='off${i}m' min='0' max='59' disabled>دقیقه
   <input type='number' id='off${i}s' min='0' max='59' disabled>ثانیه
  </label>
  <label>تعداد تکرار <input type='number' id='bc${i}' min='1' max='255' step='1' disabled> بار</label>
  <label class='wd'><input type='checkbox' id='bf${i}' onchange='toggleForever("${i}")' disabled> تکرار دائمی</label>
 </div>
 <div class='f'><button class='sv' onclick='save(${r},${k})'>ذخیره</button><button class='cl' onclick='clr(${r},${k})'>حذف</button><span class='msg' id='ms${i}'></span></div>
 <div class='wire' id='wr${i}'>12-byte record: --</div>
 </div>`;
}

function toggleAllDays(i){
 const checked=$('wa'+i).checked;
 DAYS.forEach(x=>{$('wd'+i+'_'+x[1]).checked=checked;});
}

function dayChanged(i){
 const all=DAYS.every(x=>$('wd'+i+'_'+x[1]).checked);
 $('wa'+i).checked=all;
}

function sync(i){
 const enabled=$('be'+i).checked;
 ['off'+i+'h','off'+i+'m','off'+i+'s','bc'+i,'bf'+i].forEach(id=>{$(id).disabled=!enabled;});
 $('bc'+i).disabled=!enabled||$('bf'+i).checked;
 $('offrow'+i).style.opacity=enabled?'1':'0.5';
}

function toggleForever(i){
 const forever=$('bf'+i).checked;
 const count=$('bc'+i);
 if(forever){
  const current=Number(count.value);
  if(Number.isInteger(current)&&current>=1&&current<=254)count.dataset.previous=current;
  count.value=255;
 }else{
  const previous=Number(count.dataset.previous||1);
  count.value=(previous>=1&&previous<=254)?previous:1;
 }
 count.disabled=!$('be'+i).checked||forever;
}

function wireInfo(r,k,t){
 const enabled=!!t.enabled;
 const timerId=((r&15)<<4)|(k&1);
 const kind=!enabled?0:(+t.mode===1?2:1);
 const status=!enabled?0:(t.running?5:4);
 const weekMask=enabled?(+t.weekMask||0):0;
 const start=pad(+t.hour||0)+':'+pad(+t.minute||0);
 const on=enabled?(+t.mode===1?(+t.blinkOn||0):(+t.duration||0)):0;
 const off=enabled&&+t.mode===1?(+t.blinkOff||0):0;
 const count=enabled&&+t.mode===1?(+t.blinkCount||0):0;
 $('wr'+r+'_'+k).textContent='12B | ID=0x'+timerId.toString(16).padStart(2,'0').toUpperCase()+
  ' | Type/Status=0x'+((status<<4)|kind).toString(16).padStart(2,'0').toUpperCase()+
  ' | WeekMask=0x'+weekMask.toString(16).padStart(2,'0').toUpperCase()+
  ' | Start='+start+' | TimeOn='+on+'s | TimeOff='+off+'s | Count='+count;
}

function fill(r,k,t){
 const i=r+'_'+k;
 const L=conv(t.hour,t.minute,0,-1);
 $('sh'+i).value=L.h;
 $('sm'+i).value=L.m;
 const wm=rot((t.weekMask&128)?127:t.weekMask,L.ds);
 DAYS.forEach(x=>{$('wd'+i+'_'+x[1]).checked=!!(wm&(1<<x[1]));});
 dayChanged(i);
 $('be'+i).checked=(+t.mode===1);
 fillHms('on'+i,(+t.mode===1?(t.blinkOn||0):(t.duration||0)));
 fillHms('off'+i,t.blinkOff||BMIN);
 $('bc'+i).value=t.blinkCount||1;
 $('bf'+i).checked=(+t.blinkCount===255);
 $('bc'+i).dataset.previous=(+t.blinkCount>0&&+t.blinkCount<255)?t.blinkCount:1;
 let s=t.enabled?'':'— خالی —';
 if(t.enabled&&t.running)s+=' ▶ در حال اجرا';
 if(t.enabled&&t.relay!=r)s+=' ⚠ این اسلات برای رله '+(t.relay+1)+' تنظیم شده؛ با ذخیره به این رله منتقل می‌شود';
 $('st'+i).textContent=s;
 wireInfo(r,k,t);
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
 const i=r+'_'+k;
 const h=+$('sh'+i).value,mi=+$('sm'+i).value;
 if(!Number.isInteger(h)||!Number.isInteger(mi)||h<0||h>23||mi<0||mi>59){msg(i,'ساعت باید ۰ تا ۲۳ و دقیقه ۰ تا ۵۹ باشد',0);return;}
 let localMask=0;
 DAYS.forEach(x=>{if($('wd'+i+'_'+x[1]).checked)localMask|=1<<x[1];});
 if(!localMask){msg(i,'حداقل یک روز هفته را انتخاب کنید',0);return;}
 const utc=conv(h,mi,0,1);
 const weekMask=(localMask===127)?128:rot(localMask,utc.ds);
 const on=hms('on'+i);
 const blinking=$('be'+i).checked;
 let q='slot='+(r*SLOTS+k)+'&enabled=1&type=2&year=0&month=1&day=1'+
  '&hour='+utc.h+'&minute='+utc.m+'&second=0&weekMask='+weekMask+
  '&relay='+r+'&mode='+(blinking?1:0)+'&state=1';
 if(!blinking){
  q+='&duration='+on;
 }else{
  const off=hms('off'+i);
  const rawCount=$('bc'+i).value;
  const count=$('bf'+i).checked?255:Number(rawCount);
  if(on<BMIN||on>BMAX||off<BMIN||off>BMAX){msg(i,'مدت روشن و خاموش باید بین '+BMIN+' تا '+BMAX+' ثانیه باشد',0);return;}
  if(!Number.isInteger(count)||count<1||count>255){msg(i,'تعداد تکرار باید عدد صحیح بین ۱ تا ۲۵۵ باشد.',0);return;}
  if(!$('bf'+i).checked && (!rawCount.trim() || Number(rawCount)>255)){msg(i,'تعداد تکرار نمی‌تواند بیشتر از ۲۵۵ باشد؛ برای تکرار نامحدود گزینه «تکرار دائمی» را فعال کنید.',0);return;}
  q+='&blinkOn='+on+'&blinkOff='+off+'&blinkerCount='+count;
 }
 send(q,r,i,'✓ ذخیره شد');
}

function clr(r,k){
 send('slot='+(r*SLOTS+k)+'&enabled=0&type=2&year=0&month=1&day=1&hour=0&minute=0&second=0&weekMask=0&relay='+r+'&mode=0&state=1&duration=0',
  r,r+'_'+k,'✓ حذف شد');
}

function openPanel(r){
 let p=$('pn'+r);
 if(!p){
  p=document.createElement('div');p.id='pn'+r;p.className='panel';
  let h=`<h3>تنظیمات رله ${r+1}</h3>`;
  for(let k=0;k<SLOTS;k++)h+=slotHTML(r,k);
  h+=`<div class='note'>برای هر رله دقیقاً ۲ تایمر در نظر گرفته شده است. زمان شروع بر اساس ساعت محلی مرورگر دریافت و برای ذخیره به UTC تبدیل می‌شود؛ ثانیه‌ی شروع همیشه صفر است.<br>
  در حالت عادی، رله در زمان تعیین‌شده روشن می‌شود و پس از مدت روشن بودن خاموش می‌شود. با فعال کردن گزینه‌ی چشمک‌زن، مدت روشن و خاموش و تعداد تکرار اجرا می‌شود؛ برای تکرار نامحدود، گزینه «تکرار دائمی» را فعال کنید. تعداد تکرار دستی باید عدد صحیح بین ۱ تا ۲۵۵ باشد.<br>
  همه‌ی رکوردهای تایمر در هر گزارش HTTP POST ارسال می‌شوند. خلاصه‌ی رکورد ۱۲ بایتی هر تایمر زیر تنظیمات آن نمایش داده می‌شود.<br>
  اجرای خودکار فقط وقتی فعال است که ساعت دستگاه Sync شده باشد. پس از ریست، رله‌ها خاموش می‌مانند مگر تایمر معتبر وضعیت دیگری را لازم بداند.</div>`;
  p.innerHTML=h;$('panels').appendChild(p);
 }
 const show=p.style.display!='block';
 document.querySelectorAll('.panel').forEach(x=>x.style.display='none');
 if(show){p.style.display='block';load(r);}
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
""
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
        json += F(",\"timerId\":");   json += (int)t.timerId;
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
