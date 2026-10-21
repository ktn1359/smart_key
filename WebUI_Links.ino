//======================================================
// WEB UI: صفحه‌ی «لینک‌ها و Ruleها»  (آدرس:  /links )
//======================================================
//  - تب «گروه‌ها»   : عضویت هر رله در گروه‌های PeerLink  (/getGroups  /setGroup  /delGroup)
//  - تب «Ruleها»    : ساخت و ویرایش struct Rule          (/getRulesFull  /setRule)
//  - تب «دستگاه‌ها» : Peerهای شناخته‌شده                 (/peerlinkTarget)
//  دکمه‌ی ورود به این صفحه توی صفحه‌ی اصلی (handleRootV2 در WebUI_Timers.ino) هست.
//  ثبت مسیرها (فایل اصلی):
//      server.on("/links",        handleLinksPage);
//      server.on("/getRulesFull", handleGetRulesFull);

//------------------------------------------------------
// بخش ثابت اول صفحه (HTML head + CSS) — در Flash
//------------------------------------------------------
const char PAGE_LINKS_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Links &amp; Rules</title>
<style>
body{font-family:Arial;background:#f0f0f0;margin:0;padding:12px;direction:rtl;text-align:right;}
h2{margin:6px 0 10px;text-align:center;}
.top{max-width:760px;margin:0 auto;}
.bk{color:#2980b9;text-decoration:none;font-size:14px;}
.tabs{max-width:760px;margin:8px auto;display:flex;gap:6px;}
.tabs button{flex:1;padding:9px 4px;font-size:15px;border:0;border-radius:8px;background:#bdc3c7;color:#fff;cursor:pointer;}
.tabs button.act{background:#34495e;}
.sec{display:none;max-width:760px;margin:0 auto;}
.card,.slot{background:#fff;border-radius:10px;padding:10px 12px;margin:8px 0;box-shadow:0 1px 5px #0003;}
.slot{border:1px solid #ddd;box-shadow:none;margin:6px 0;}
.hd{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap;}
.f{display:flex;flex-wrap:wrap;gap:6px 12px;align-items:center;margin:7px 0;}
.note{font-size:12px;color:#666;line-height:1.8;margin:4px 0;}
.sm{font-size:14px;line-height:1.8;margin-top:4px;}
input,select{font-size:14px;padding:3px 4px;margin:0 2px;}
input[type=number]{width:80px;}
.sv,.cl{color:#fff;border:0;border-radius:6px;padding:6px 14px;cursor:pointer;font-size:14px;}
.sv{background:#2980b9;}
.cl{background:#7f8c8d;}
.ok{color:#27ae60;font-size:13px;}
.off{color:#e74c3c;font-size:13px;}
.bad{color:#c0392b;font-size:13px;}
.msg{font-size:13px;}
table{width:100%;border-collapse:collapse;font-size:14px;}
th,td{border-bottom:1px solid #eee;padding:5px 4px;text-align:right;}
</style></head><body>
<div class='top'><a class='bk' href='/'>&larr; صفحه‌ی اصلی</a><h2>لینک‌ها و Ruleها</h2></div>
<div class='tabs'><button id='tb0' onclick='tab(0)'>گروه‌ها</button><button id='tb1' onclick='tab(1)'>Ruleها</button><button id='tb2' onclick='tab(2)'>دستگاه‌ها</button></div>
<div id='t0' class='sec'></div>
<div id='t1' class='sec'><div id='rl'></div><div id='ed'></div></div>
<div id='t2' class='sec'></div>
)rawliteral";

//------------------------------------------------------
// بخش ثابت آخر صفحه (JavaScript) — در Flash
//------------------------------------------------------
const char PAGE_LINKS_TAIL[] PROGMEM = R"rawliteral(<script>
function $(i){return document.getElementById(i);}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
function get(u){return fetch(u).then(function(r){return r.text().then(function(t){if(!r.ok)throw new Error(t||('HTTP '+r.status));return t;});});}
function getj(u){return get(u).then(JSON.parse);}
function nv(id){return +($(id).value||0);}
function say(id,t,ok){var e=$(id);if(!e)return;e.style.color=ok?'#27ae60':'#c0392b';e.textContent=t;}
var G=null,P=[],RL=[],EDIT=-1;

function tab(n){
for(var i=0;i<3;i++){$('t'+i).style.display=i==n?'block':'none';$('tb'+i).className=i==n?'act':'';}
if(n==0)loadG();else if(n==1)loadR();else loadP();
}

// ================= دستگاه‌ها =================
function loadP(){
return getj('/peerlinkTarget').then(function(a){P=a;renderP();}).catch(function(){$('t2').innerHTML="<div class='card'>خطا در ارتباط با دستگاه</div>";});
}
function renderP(){
var h="<div class='card'><b>دستگاه‌های شناخته‌شده</b><div class='note'>دستگاه‌ها با Secret یکسان خودکار پیدا می‌شوند (بعد از ANNOUNCE).</div><table><tr><th>#</th><th>شناسه</th><th>IP</th><th>وضعیت</th></tr>";
P.forEach(function(p){if(!p.used)return;
h+="<tr><td>"+p.index+"</td><td dir='ltr'>"+esc(p.deviceId)+"</td><td dir='ltr'>"+esc(p.ip)+"</td><td>"+(p.online?"<span class='ok'>آنلاین</span>":"<span class='bad'>آفلاین</span>")+"</td></tr>";});
h+="</table><div class='f'><label>ثبت دستی (شناسه‌ی ۱۲ رقمی) <input id='pid' maxlength='12' dir='ltr'></label><button class='sv' onclick='addP()'>ثبت</button><span class='msg' id='pm'></span></div></div>";
$('t2').innerHTML=h;
}
function addP(){
var v=$('pid').value.trim();
if(!v){say('pm','شناسه را وارد کنید',0);return;}
get('/peerlinkTarget?deviceId='+encodeURIComponent(v)).then(function(){loadP().then(function(){say('pm','✓ ثبت شد',1);});}).catch(function(e){say('pm',e.message,0);});
}

// ================= گروه‌ها =================
function loadG(){
return Promise.all([getj('/getGroups'),getj('/peerlinkTarget')]).then(function(x){G=x[0];P=x[1];renderG();}).catch(function(){$('t0').innerHTML="<div class='card'>خطا در ارتباط با دستگاه (یا این فریمور گروه‌ها را ندارد)</div>";});
}
function renderG(){
var h="<div class='card note'>هر <b>گروه</b> فقط یک شماره است و کلیدهای هم‌شماره به هم وصل می‌شوند:<br>"+
"• <b>فرمان‌ده</b>: تغییرش را برای اعضای گروه می‌فرستد و تغییر گروه را هم می‌گیرد (فرمان‌ده‌های یک گروه هم‌گام‌اند: کلید تبدیل / صلیبی).<br>"+
"• <b>شنونده</b>: فقط از گروه فرمان می‌گیرد و چیزی نمی‌فرستد (مثل کلیدهای یک سناریو).<br>"+
"هر کلید فقط در یک گروه «فرمان‌ده» است ولی می‌تواند در چند گروه شنونده باشد. همین شماره‌ی گروه را روی همه‌ی دستگاه‌های مرتبط هم بگذار.</div>";
G.relays.forEach(function(R){
h+="<div class='card'><div class='hd'><b>رله "+(R.relay+1)+"</b><span class='"+(R.state?"ok":"off")+"'>"+(R.state?"روشن":"خاموش")+"</span></div>";
if(!R.groups.length)h+="<div class='note'>عضو هیچ گروهی نیست</div>";
R.groups.forEach(function(g){
h+="<div class='f'><label>گروه <b dir='ltr'>"+g.group+"</b></label>"+
"<select onchange='gset("+R.relay+","+g.group+",this.value)'><option value='1'"+(g.role==1?" selected":"")+">فرمان‌ده</option><option value='0'"+(g.role==0?" selected":"")+">شنونده</option></select>"+
"<button class='cl' onclick='gdel("+R.relay+","+g.group+")'>حذف</button></div>";});
h+="<div class='f'><label>گروه جدید <input type='number' id='gn"+R.relay+"' min='1' style='width:110px' dir='ltr'></label>"+
"<button class='cl' onclick='grand("+R.relay+")'>شماره‌ی تصادفی</button>"+
"<select id='gro"+R.relay+"'><option value='1'>فرمان‌ده</option><option value='0'>شنونده</option></select>"+
"<button class='sv' onclick='gadd("+R.relay+")'>افزودن</button><span class='msg' id='gm"+R.relay+"'></span></div></div>";
});
$('t0').innerHTML=h;
}
function gdone(r,t,ok){loadG().then(function(){say('gm'+r,t,ok);});}
function gset(r,g,role){get('/setGroup?relay='+r+'&group='+g+'&role='+role).then(function(){gdone(r,'✓ ذخیره شد',1);}).catch(function(e){loadG().then(function(){say('gm'+r,e.message,0);});});}
function gdel(r,g){if(!confirm('این عضویت حذف شود؟'))return;get('/delGroup?relay='+r+'&group='+g).then(function(){gdone(r,'✓ حذف شد',1);}).catch(function(e){say('gm'+r,e.message,0);});}
function grand(r){$('gn'+r).value=100000+Math.floor(Math.random()*900000);}
function gadd(r){
var n=+$('gn'+r).value;
if(!(n>=1)||n!=Math.floor(n)){say('gm'+r,'شماره‌ی گروه باید عدد صحیح مثبت باشد',0);return;}
gset(r,n,$('gro'+r).value);
}

// ================= Ruleها =================
function used(r){return r.enabled||r.triggerType||r.actionType;}
function loadR(){
return Promise.all([getj('/getRulesFull'),getj('/peerlinkTarget')]).then(function(x){RL=x[0];P=x[1];renderR();}).catch(function(){$('rl').innerHTML="<div class='card'>خطا در ارتباط با دستگاه (یا این فریمور /getRulesFull ندارد)</div>";});
}
function relN(i){return 'رله '+(+i+1);}
function peerN(i){var p=P[i];return (p&&p.used)?(p.ip||p.deviceId):('#'+i);}
function sumR(r){
var t='';
if(r.triggerType==1)t+='وقتی '+relN(r.triggerSource)+(r.triggerValue?' روشن':' خاموش')+' شد';
else if(r.triggerType==2)t+='وقتی سناریوی '+r.triggerScenarioId+' اجرا شد';
else t+='(بدون Trigger)';
if(r.conditionType==1)t+=' و '+relN(r.conditionSource)+(r.conditionValue?' روشن':' خاموش')+' بود';
t+='، ';
t+=(r.actionIsRemote?relN(r.actionTarget)+' دستگاه '+peerN(r.actionPeerIndex):relN(r.actionTarget)+' همین دستگاه')+' ';
t+=['(بدون عمل)','روشن شود','خاموش شود','Toggle شود'][r.actionType]||'';
if(r.actionDelaySeconds)t+=' (بعد از '+r.actionDelaySeconds+' ثانیه)';
return t;
}
function renderR(){
var h="<div class='card note'>Rule یعنی: «وقتی ... شد، اگر ... بود، این کار انجام شود». برای وصل کردن کلیدها به هم از تب «گروه‌ها» استفاده کن؛ Rule برای کارهای شرطی، تأخیری و محلی است. لینک‌های قدیمیِ ON/OFF (هر لینک دو Rule) هم همین‌جا دیده می‌شوند.</div>";
var cnt=0;
RL.forEach(function(r,s){
if(!used(r))return;cnt++;
h+="<div class='slot'><div class='hd'><b>اسلات "+(s+1)+"</b><span class='"+(r.enabled?"ok":"off")+"'>"+(r.enabled?"فعال":"غیرفعال")+"</span><button class='cl' onclick='editR("+s+")'>ویرایش</button></div><div class='sm'>"+esc(sumR(r))+"</div></div>";
});
if(!cnt)h+="<div class='card note'>هیچ Ruleی تعریف نشده</div>";
h+="<div class='f'><button class='sv' onclick='newR()'>+ Rule جدید</button><span class='note'>"+cnt+" از "+MR+" اسلات استفاده شده</span></div>";
h+="<div class='f'><label>تست سناریو: شماره <input type='number' id='fs' min='0' max='255'></label><button class='cl' onclick='fireS()'>اجرا</button><span class='msg' id='fm'></span></div>";
$('rl').innerHTML=h;
}
function fireS(){var v=$('fs').value;if(v===''){say('fm','شماره‌ی سناریو را وارد کنید',0);return;}get('/fireScenario?id='+v).then(function(){say('fm','✓ اجرا شد',1);}).catch(function(e){say('fm',e.message,0);});}
function newR(){var f=-1;for(var i=0;i<RL.length;i++){if(!used(RL[i])){f=i;break;}}if(f<0){alert('اسلات خالی نیست');return;}editR(f);}
function relOpts(sel){var h='';for(var i=0;i<NR;i++)h+="<option value='"+i+"'"+(i==sel?" selected":"")+">رله "+(i+1)+"</option>";return h;}
function peerOpts(sel){var h='';P.forEach(function(p){if(p.used)h+="<option value='"+p.index+"'"+(p.index==sel?" selected":"")+">"+esc(p.ip||p.deviceId)+" ("+(p.online?"آنلاین":"آفلاین")+")</option>";});return h||"<option value=''>هیچ دستگاهی نیست</option>";}
function editR(s){
EDIT=s;var r=RL[s];
var h="<div class='card'><div class='hd'><b>اسلات "+(s+1)+"</b><label><input type='checkbox' id='e_en'> فعال</label></div>"+
"<div class='f'><b>شرط شروع (Trigger)</b><select id='e_tt' onchange='syncE()'><option value='1'>وضعیت یک رله</option><option value='2'>سناریو</option></select>"+
"<span id='e_t1'><select id='e_ts'>"+relOpts(r.triggerSource)+"</select><select id='e_tv'><option value='1'>روشن شد</option><option value='0'>خاموش شد</option></select></span>"+
"<span id='e_t2'>شماره‌ی سناریو <input type='number' id='e_si' min='0' max='255'></span></div>"+
"<div class='f'><b>شرط اضافه (Condition)</b><select id='e_ct' onchange='syncE()'><option value='0'>بدون شرط</option><option value='1'>وضعیت یک رله</option></select>"+
"<span id='e_c1'><select id='e_cs'>"+relOpts(r.conditionSource)+"</select><select id='e_cv'><option value='1'>روشن باشد</option><option value='0'>خاموش باشد</option></select></span></div>"+
"<div class='f'><b>عمل (Action)</b><select id='e_at'><option value='1'>روشن شود</option><option value='2'>خاموش شود</option><option value='3'>Toggle شود</option></select>"+
"<select id='e_ar'>"+relOpts(r.actionTarget)+"</select>"+
"<select id='e_ad' onchange='syncE()'><option value='0'>روی همین دستگاه</option><option value='1'>روی دستگاه دیگر</option></select>"+
"<span id='e_ap'><select id='e_pi'>"+peerOpts(r.actionPeerIndex)+"</select></span></div>"+
"<div class='f'><label>تأخیر <input type='number' id='e_dl' min='0' max='"+XD+"' style='width:90px'> ثانیه</label><span class='note'>(۰ = فوری؛ در غیر این صورت "+MD+" تا "+XD+" ثانیه)</span></div>"+
"<div class='f'><button class='sv' onclick='saveR()'>ذخیره</button><button class='cl' onclick='clrR()'>حذف</button><button class='cl' onclick='closeE()'>بستن</button><span class='msg' id='em'></span></div>"+
"<div class='note'>اگر عمل روی «دستگاه دیگر» باشد فقط «روشن/خاموش» پشتیبانی می‌شود. برای یک لینک کامل دو Rule لازم است (یکی روشن شد ← روشن، یکی خاموش شد ← خاموش) یا بهتر از تب «گروه‌ها» استفاده کن.</div></div>";
$('ed').innerHTML=h;
$('e_en').checked=!!r.enabled;
$('e_tt').value=(r.triggerType==2)?2:1;
$('e_tv').value=r.triggerValue?1:0;
$('e_si').value=r.triggerScenarioId||0;
$('e_ct').value=r.conditionType==1?1:0;
$('e_cv').value=r.conditionValue?1:0;
$('e_at').value=(r.actionType>=1&&r.actionType<=3)?r.actionType:1;
$('e_ad').value=r.actionIsRemote?1:0;
$('e_dl').value=r.actionDelaySeconds||0;
syncE();
$('ed').scrollIntoView();
}
function closeE(){$('ed').innerHTML='';EDIT=-1;}
function syncE(){
var tt=$('e_tt').value,ct=$('e_ct').value,rem=$('e_ad').value==1;
$('e_t1').style.display=tt==1?'':'none';
$('e_t2').style.display=tt==2?'':'none';
$('e_c1').style.display=ct==1?'':'none';
$('e_ap').style.display=rem?'':'none';
$('e_at').options[2].disabled=rem;
if(rem&&$('e_at').value==3)$('e_at').value=1;
}
function saveR(){
var s=EDIT,tt=+$('e_tt').value,ct=+$('e_ct').value,rem=+$('e_ad').value,dl=+($('e_dl').value||0);
if(dl!=0&&(dl<MD||dl>XD)){say('em','تأخیر باید ۰ یا بین '+MD+' تا '+XD+' ثانیه باشد',0);return;}
if(rem&&$('e_pi').value===''){say('em','دستگاه مقصد را انتخاب کنید',0);return;}
var q='slot='+s+'&enabled='+($('e_en').checked?1:0)+
'&triggerType='+tt+'&triggerSource='+nv('e_ts')+'&triggerValue='+$('e_tv').value+'&triggerScenarioId='+(+($('e_si').value||0))+
'&conditionType='+ct+'&conditionSource='+nv('e_cs')+'&conditionValue='+$('e_cv').value+
'&actionType='+$('e_at').value+'&actionTarget='+nv('e_ar')+'&actionIsRemote='+rem+'&actionPeerIndex='+(rem?nv('e_pi'):0)+'&actionDelaySeconds='+dl;
get('/setRule?'+q).then(function(){loadR().then(function(){say('em','✓ ذخیره شد',1);});}).catch(function(e){say('em',e.message,0);});
}
function clrR(){
if(!confirm('این Rule حذف شود؟'))return;
var q='slot='+EDIT+'&enabled=0&triggerType=0&triggerSource=0&triggerValue=0&triggerScenarioId=0&conditionType=0&conditionSource=0&conditionValue=0&actionType=0&actionTarget=0&actionIsRemote=0&actionPeerIndex=0&actionDelaySeconds=0';
get('/setRule?'+q).then(function(){closeE();loadR();}).catch(function(e){say('em',e.message,0);});
}
window.onload=function(){tab(0);};
</script></body></html>
)rawliteral";

//------------------------------------------------------
// صفحه‌ی /links   (server.on("/links", handleLinksPage))
//------------------------------------------------------
void handleLinksPage()
{
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    server.sendContent_P(PAGE_LINKS_HEAD);

    // ثابت‌های لازم برای جاوااسکریپت (از #define های فرمور)
    String s = F("<script>const NR=");
    s += TOUCH_COUNT;
    s += F(",MR=");
    s += MAX_RULES;
    s += F(",MD=");
    s += (unsigned long)MIN_RULE_DELAY_SECONDS;
    s += F(",XD=");
    s += (unsigned long)MAX_RULE_DELAY_SECONDS;
    s += F(";</script>");
    server.sendContent(s);

    server.sendContent_P(PAGE_LINKS_TAIL);
    server.sendContent("");   // پایان پاسخ (chunked)
}

//------------------------------------------------------
// /getRulesFull : همه‌ی فیلدهای struct Rule (شامل Remote و سناریو)
// (/getRules قدیمی دست‌نخورده باقی مانده)
//------------------------------------------------------
void handleGetRulesFull()
{
    String json;
    json.reserve(MAX_RULES * 270 + 8);
    json += '[';

    for (uint8_t i = 0; i < MAX_RULES; i++)
    {
        Rule &r = rules[i];

        if (i > 0)
            json += ',';

        json += F("{\"slot\":");               json += (int)i;
        json += F(",\"enabled\":");            json += (r.enabled ? 1 : 0);
        json += F(",\"triggerType\":");        json += (int)r.triggerType;
        json += F(",\"triggerSource\":");      json += (int)r.triggerSource;
        json += F(",\"triggerValue\":");       json += (r.triggerValue ? 1 : 0);
        json += F(",\"triggerScenarioId\":");  json += (int)r.triggerScenarioId;
        json += F(",\"lastTriggerState\":");   json += (r.lastTriggerState ? 1 : 0);
        json += F(",\"conditionType\":");      json += (int)r.conditionType;
        json += F(",\"conditionSource\":");    json += (int)r.conditionSource;
        json += F(",\"conditionValue\":");     json += (r.conditionValue ? 1 : 0);
        json += F(",\"actionType\":");         json += (int)r.actionType;
        json += F(",\"actionTarget\":");       json += (int)r.actionTarget;
        json += F(",\"actionIsRemote\":");     json += (r.actionIsRemote ? 1 : 0);
        json += F(",\"actionPeerIndex\":");    json += (int)r.actionPeerIndex;
        json += F(",\"actionDelaySeconds\":"); json += (unsigned long)r.actionDelaySeconds;
        json += '}';
    }

    json += ']';

    server.send(200, "application/json", json);
}
