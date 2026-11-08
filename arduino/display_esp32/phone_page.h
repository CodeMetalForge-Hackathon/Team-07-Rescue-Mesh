// phone_page.h - the page a phone gets when it joins the box's Wi-Fi (served by display_esp32.ino).
// Plain HTML + JavaScript, no internet needed. It polls /api/state and posts to /api/send and /api/quick.
#pragma once

const char PHONE_PAGE[] = R"PAGE(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Team 7 box</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:-apple-system,system-ui,sans-serif;background:#0d1117;color:#e6edf3;display:flex;flex-direction:column;height:100vh;height:100dvh}
header{padding:10px 14px;background:#161b22;border-bottom:1px solid #30363d;display:flex;align-items:center;gap:10px}
#name{font-weight:700;font-size:18px}
.pill{font-size:12px;font-weight:700;padding:3px 8px;border-radius:10px;background:#30363d}
.CARRY{background:#d97706;color:#000}.DEPLOY{background:#16a34a;color:#000}.BASE{background:#2563eb}
#env{margin-left:auto;font-size:14px;color:#8b949e}
#boxes{padding:6px 14px;font-size:13px;color:#8b949e;border-bottom:1px solid #30363d;white-space:nowrap;overflow-x:auto}
#boxes b{color:#e6edf3;font-weight:600}
.banner{padding:10px 14px;font-weight:700;text-align:center}
#warn{background:#b91c1c;display:none}
#nolink{background:#7f1d1d;display:none}
#list{flex:1;overflow-y:auto;padding:10px 12px}
.m{max-width:85%;margin:6px 0;padding:8px 11px;border-radius:14px;background:#21262d;word-wrap:break-word}
.m.me{margin-left:auto;background:#1f6feb}
.m.help{background:#b91c1c}
.meta{font-size:11px;color:#8b949e;margin-bottom:2px}
.me .meta{color:#c9d8f0}
.st{font-size:11px;margin-top:3px;text-align:right}
.ok{color:#7ee787}.no{color:#ffa198}.wait{color:#e3b341}
#empty{color:#8b949e;text-align:center;margin-top:30px}
#quick{display:flex;gap:10px;padding:10px 12px 0}
#quick button{flex:1;padding:16px 0;font-size:18px;font-weight:800;border:0;border-radius:12px;color:#fff}
#help{background:#dc2626}#help.armed{background:#fff;color:#dc2626}
#okb{background:#2563eb}
form{display:flex;gap:8px;padding:10px 12px 14px}
select,input,form button{font-size:16px;border-radius:10px;border:1px solid #30363d;background:#0d1117;color:#e6edf3;padding:10px}
select{max-width:30%}
input{flex:1;min-width:0}
form button{background:#238636;border:0;font-weight:700}
#count{font-size:11px;color:#8b949e;text-align:right;padding:0 14px}
#toast{position:fixed;left:50%;bottom:150px;transform:translateX(-50%);background:#30363d;padding:8px 14px;border-radius:10px;display:none}
</style></head><body>
<header><span id="name">Team 7</span><span id="mode" class="pill">...</span><span id="env"></span></header>
<div id="boxes">connecting...</div>
<div id="warn" class="banner"></div>
<div id="nolink" class="banner">Screen board is not hearing the radio box</div>
<div id="list"><div id="empty">No messages yet</div></div>
<div id="quick"><button id="help" type="button">HELP</button><button id="okb" type="button">OK</button></div>
<form id="f"><select id="to"><option value="*">Everyone</option></select>
<input id="t" maxlength="60" placeholder="Message (60 max)" autocomplete="off"><button>Send</button></form>
<div id="count"></div>
<div id="toast"></div>
<script>
var $=function(i){return document.getElementById(i)},lastN=-1,armed=0,known={};
function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function ago(s){return s<60?s+'s':s<3600?Math.floor(s/60)+'m':Math.floor(s/3600)+'h'}
function bars(r){var m=r+123;return m>45?'▂▄▆█':m>30?'▂▄▆':m>15?'▂▄':m>5?'▂':'·'}
function toast(t){var e=$('toast');e.textContent=t;e.style.display='block';clearTimeout(e.h);e.h=setTimeout(function(){e.style.display='none'},2000)}
function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(b)})
 .then(function(r){toast(r.ok?'Sent to the box':'Box busy - try again');setTimeout(poll,400)}).catch(function(){toast('Not connected')})}
function render(s){
 $('name').textContent=s.id;var md=$('mode');md.textContent=s.mode||'...';md.className='pill '+s.mode;
 $('env').textContent=(s.temp!='--'?s.temp+'°C ':'')+(s.rh!='--'?s.rh+'%':'');
 var w=$('warn');w.style.display=s.warn?'block':'none';w.textContent=s.warn.replace('|',' — ');
 $('nolink').style.display=s.link?'none':'block';
 var b=s.boxes.filter(function(x){return x.age<75});
 $('boxes').innerHTML=b.length?'Hears: '+b.map(function(x){return '<b>'+esc(x.id)+'</b> '+bars(x.rssi)}).join(' &nbsp; '):'No other boxes heard';
 b.forEach(function(x){known[x.id]=1});s.msgs.forEach(function(m){if(!m.mine)known[m.from]=1});
 var sel=$('to'),cur=sel.value,ids=Object.keys(known).sort();
 if(sel.options.length!=ids.length+1){sel.innerHTML='<option value="*">Everyone</option>'+ids.map(function(i){return '<option>'+esc(i)+'</option>'}).join('');sel.value=cur}
 $('count').textContent=s.waiting?'Waiting for the box to take '+s.waiting+' message(s)...':'';
 var L=$('list'),atBottom=L.scrollHeight-L.scrollTop-L.clientHeight<40,h='';
 s.msgs.forEach(function(m){
  var help=!m.mine&&m.text=='[HELP]',st='';
  if(m.mine)st=m.ack>0?'<div class="st ok">✓ '+esc(m.by)+(m.n>1?' +'+(m.n-1):'')+'</div>':m.ack<0?'<div class="st no">no reply yet</div>':'<div class="st wait">sent \u00b7 waiting for reply</div>';
  h+='<div class="m'+(m.mine?' me':'')+(help?' help':'')+'"><div class="meta">'+esc(m.mine?'You':m.from)+' · '+ago(m.age)+'</div>'+esc(m.text)+st+'</div>'});
 L.innerHTML=h||'<div id="empty">No messages yet</div>';
 if(atBottom||s.msgs.length!=lastN)L.scrollTop=L.scrollHeight;lastN=s.msgs.length}
function poll(){fetch('/api/state',{cache:'no-store'}).then(function(r){return r.json()}).then(render).catch(function(){$('boxes').textContent='Not connected to the box Wi-Fi'})}
$('help').onclick=function(){var b=this;if(!armed){armed=1;b.classList.add('armed');b.textContent='TAP AGAIN';setTimeout(function(){armed=0;b.classList.remove('armed');b.textContent='HELP'},3000);return}
 armed=0;b.classList.remove('armed');b.textContent='HELP';post('/api/quick',{code:'HELP'})};
$('okb').onclick=function(){post('/api/quick',{code:'OK'})};
$('f').onsubmit=function(e){e.preventDefault();var t=$('t').value.trim();if(!t)return;post('/api/send',{to:$('to').value,text:t});$('t').value=''};
poll();setInterval(poll,1500);
</script></body></html>)PAGE";
