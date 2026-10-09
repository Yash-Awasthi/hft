#pragma once

// Dashboard served by pm_live at "/": polls /metrics.json once a second and draws the last
// five minutes with plain canvas code, so it needs nothing from the network.

#include <string_view>

namespace hft::pm {

inline constexpr std::string_view kDashboard = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>pm_live</title>
<style>
:root{--bg:#f7f7f5;--panel:#fff;--ink:#1d1d1b;--mute:#6b6b66;--line:#e2e2dc;--a:#2f6fdf;--b:#d9822b;--good:#2e8b57;--bad:#c0392b}
@media (prefers-color-scheme:dark){:root{--bg:#141413;--panel:#1d1d1b;--ink:#ecebe6;--mute:#9a9a92;--line:#2f2f2c;--a:#6f9cf0;--b:#e7a35c;--good:#5cbf8a;--bad:#e5735f}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.4 system-ui,sans-serif}
header{display:flex;flex-wrap:wrap;gap:8px 24px;align-items:baseline;padding:16px}
h1{font-size:18px;margin:0}.mute{color:var(--mute)}
main{display:grid;gap:12px;padding:0 16px 16px;grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}
section{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px;min-width:0}
h2{font-size:13px;font-weight:600;margin:0 0 8px;color:var(--mute);text-transform:uppercase;letter-spacing:.04em}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px;grid-column:1/-1}
.kpi{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px 12px}
.kpi b{display:block;font-size:20px;font-variant-numeric:tabular-nums}
canvas{width:100%;height:140px;display:block}
.wide{grid-column:1/-1}table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
.scroll{overflow-x:auto}th,td{text-align:right;padding:4px 6px;border-bottom:1px solid var(--line);white-space:nowrap}
th:first-child,td:first-child{text-align:left;max-width:320px;overflow:hidden;text-overflow:ellipsis}
.key{display:inline-block;width:10px;height:2px;vertical-align:middle;margin:0 4px 0 10px}
</style></head><body>
<header><h1>pm_live</h1><span class="mute" id="status">connecting</span><span class="mute">paper trading, no orders sent</span></header>
<main>
<div class="kpis" id="kpis"></div>
<section><h2>Messages per second</h2><canvas id="c-rate"></canvas></section>
<section><h2>Wire to decision, µs<span class="key" style="background:var(--a)"></span>p50<span class="key" style="background:var(--b)"></span>p99</h2><canvas id="c-lat"></canvas></section>
<section><h2>Paper PnL, USD</h2><canvas id="c-pnl"></canvas></section>
<section><h2>Open arbitrage windows</h2><canvas id="c-arb"></canvas></section>
<section class="wide"><h2>Latency by stage this second, ns</h2><div class="scroll"><table id="t-lat"></table></div></section>
<section class="wide"><h2>Connections</h2><div class="scroll"><table id="t-conn"></table></div></section>
<section class="wide"><h2>Arbitrage groups (most windows)</h2><div class="scroll"><table id="t-arb"></table></div></section>
<section class="wide"><h2>Tokens (largest positions, then most quoted)</h2><div class="scroll"><table id="t-tok"></table></div></section>
</main>
<script>
const N=300,hist={rate:[],p50:[],p99:[],pnl:[],arb:[]};let lastMsgs=null;
const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v).trim();
function push(k,v){hist[k].push(v);if(hist[k].length>N)hist[k].shift()}
function draw(id,series){const c=document.getElementById(id),r=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
 c.width=w*r;c.height=h*r;const g=c.getContext('2d');g.scale(r,r);g.clearRect(0,0,w,h);
 const all=series.flatMap(s=>s.d);if(!all.length)return;let lo=Math.min(0,...all),hi=Math.max(...all);if(hi===lo)hi=lo+1;
 const pad=18,y=v=>h-pad-(v-lo)/(hi-lo)*(h-2*pad),x=i=>i/(N-1)*w;
 g.strokeStyle=css('--line');g.lineWidth=1;g.beginPath();g.moveTo(0,y(0));g.lineTo(w,y(0));g.stroke();
 g.fillStyle=css('--mute');g.font='11px system-ui';g.fillText(fmt(hi),2,11);g.fillText(fmt(lo),2,h-4);
 for(const s of series){g.strokeStyle=css(s.c);g.lineWidth=1.5;g.beginPath();
  s.d.forEach((v,i)=>{const X=x(N-s.d.length+i);i?g.lineTo(X,y(v)):g.moveTo(X,y(v))});g.stroke()}}
function fmt(v){const a=Math.abs(v);return a>=1e6?(v/1e6).toFixed(1)+'M':a>=1e3?(v/1e3).toFixed(1)+'k':(+v.toFixed(2)).toString()}
function table(id,head,rows){const t=document.getElementById(id);
 t.innerHTML='<tr>'+head.map(h=>'<th>'+h+'</th>').join('')+'</tr>'+rows.map(r=>'<tr>'+r.map(c=>'<td>'+c+'</td>').join('')+'</tr>').join('')}
const esc=s=>String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const px=v=>v<0?'':(v/1e4).toFixed(3);
async function tick(){try{const m=await (await fetch('/metrics.json',{cache:'no-store'})).json();
 const rate=lastMsgs===null?0:m.messages-lastMsgs;lastMsgs=m.messages;
 push('rate',rate);push('p50',m.lat.total.p50/1e3);push('p99',m.lat.total.p99/1e3);push('pnl',m.pnl);push('arb',m.arb_open);
 document.getElementById('status').textContent='up '+Math.round(m.uptime_s)+' s'+(m.halted?' · HALTED':'');
 const k=[['Messages',fmt(m.messages)],['Events',fmt(m.events)],['Trades seen',fmt(m.trades)],['Paper fills',m.fills],
  ['PnL, USD',m.pnl.toFixed(2)],['Gross shares',fmt(m.gross)],['Arb windows',m.arb_windows],['Reconnects',m.reconnects],['Ring drops',m.drops]];
 document.getElementById('kpis').innerHTML=k.map(([a,b])=>'<div class="kpi"><span class="mute">'+a+'</span><b>'+b+'</b></div>').join('');
 draw('c-rate',[{d:hist.rate,c:'--a'}]);draw('c-lat',[{d:hist.p50,c:'--a'},{d:hist.p99,c:'--b'}]);
 draw('c-pnl',[{d:hist.pnl,c:'--good'}]);draw('c-arb',[{d:hist.arb,c:'--b'}]);
 table('t-lat',['stage','p50','p99','p99.9','max','n'],Object.entries({'ring queue':m.lat.queue,'parse':m.lat.parse,'books + strategy + risk':m.lat.engine,'wire to decision':m.lat.total,'exchange stamp to receive':m.lat.exch})
  .map(([n,l])=>[n,fmt(l.p50),fmt(l.p99),fmt(l.p999),fmt(l.max),l.n]));
 table('t-conn',['connection','tokens','messages','reconnects','last message, s'],m.conns.map((c,i)=>['c'+i,c.tokens,fmt(c.messages),c.reconnects,c.age_s]));
 table('t-arb',['group','windows','open, s','max edge, 0.0001'],m.groups.map(g=>[esc(g.name),g.windows,g.open_s,g.max_edge]));
 table('t-tok',['token','book bid','book ask','our bid','our ask','inventory','PnL','quotes','on'],
  m.tokens.map(t=>[esc(t.label),px(t.best_bid),px(t.best_ask),px(t.bid),px(t.ask),t.inv,t.pnl,t.quotes,t.on?'yes':'no']));
}catch(e){document.getElementById('status').textContent='no data: '+e}}
setInterval(tick,1000);tick();
</script></body></html>
)HTML";

}  // namespace hft::pm
