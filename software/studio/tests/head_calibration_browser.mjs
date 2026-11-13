/** 独立浏览器组件验收：只运行本机假主控，不提供鸭子地址参数。用法：node 本文件 <CDP端口> [截图目录]。 */
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { resolve, dirname, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const port = Number(process.argv[2]);
assert(Number.isInteger(port) && port > 0 && port < 65536, '需要本机测试浏览器 CDP 端口');
const output = process.argv[3];
const web = resolve(dirname(fileURLToPath(import.meta.url)), '../web');
const harness = `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<style>body{background:#18191d;color:#eee;font:16px sans-serif;margin:24px;line-height:1.6}
button,input{font:inherit;margin:8px;padding:8px}button:disabled{opacity:.4}li{margin:4px 0}section{max-width:850px}</style>
<main id="root"></main><script type="module">
import {html,render} from '/vendor/preact-htm.module.js';
import {HeadCalibrationPanel} from '/head-calibration.js';
window.calls=[];window.phase={active:false};window.failNext=false;
window.fakeLink={
/** 模拟主控有序推进及一次失败；仅记录操作，不连接实物或生成真实核验证明。 */
async cmd(name,args){
    window.calls.push({name,args});
    if(args.op==='head-start')window.phase={active:true,next_step:0,reports:[],sample_count:100};
    if(args.op==='head-capture'){
        if(window.failNext){window.failNext=false;return {ok:false,reason:'模拟：躯干补偿误差过大，未保存'};}
        if(args.step!==window.phase.next_step)return {ok:false,reason:'模拟：不允许跳步'};
        window.phase.reports.push({step:args.step,sample_count:100,max_body_error_deg:.2,body_gyro_rms_rad_s:.01});
        window.phase.next_step++;window.phase.ready_to_save=window.phase.next_step===10;
    }
    if(args.op==='head-save'){
        if(window.phase.next_step!==10)return {ok:false,reason:'模拟：尚未完成'};
        window.phase={active:false,saved:true};
    }
    if(args.op==='head-cancel')window.phase={active:false,cancelled:true};
    return {ok:true,data:structuredClone(window.phase)};
}};
window.unmount=()=>render(null,document.querySelector('#root'));
render(html\`<\${HeadCalibrationPanel} imu=\${{source:'head_bmi088',mode:'commissioning'}}
    link=\${window.fakeLink} live=\${true} />\`,document.querySelector('#root'));
</script>`;

/** 只服务源码目录和固定测试 HTML；不代理网络请求。 */
const server = createServer(async (request, response) => {
    try {
        if (request.url === '/') {
            response.setHeader('content-type', 'text/html; charset=utf-8');
            response.end(harness);
            return;
        }
        const path = resolve(web, `.${new URL(request.url, 'http://localhost').pathname}`);
        assert(path.startsWith(`${web}/`));
        response.setHeader('content-type', extname(path) === '.js' ? 'text/javascript' : 'application/octet-stream');
        response.end(await readFile(path));
    } catch {
        response.writeHead(404);
        response.end();
    }
});
await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen));
const page = await (await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: 'PUT' })).json();
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolveOpen) => socket.addEventListener('open', resolveOpen, { once: true }));
let nextId = 0;
const pending = new Map();
const errors = [];
socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    pending.get(message.id)?.(message);
    if (message.method === 'Runtime.exceptionThrown') {
        errors.push(message.params.exceptionDetails.text);
    }
});

/** 通过本机调试接口发一条限时浏览器请求，异常直接让验收失败。 */
function send(method, params = {}) {
    return new Promise((resolveReply, reject) => {
        const id = ++nextId;
        const timeout = setTimeout(() => {
            pending.delete(id);
            reject(new Error(`CDP timeout: ${method}`));
        }, 15000);
        pending.set(id, (message) => {
            clearTimeout(timeout);
            pending.delete(id);
            if (message.error) {
                reject(new Error(JSON.stringify(message.error)));
            } else {
                resolveReply(message.result);
            }
        });
        socket.send(JSON.stringify({ id, method, params }));
    });
}

/** 在真实 DOM 中执行表达式；不把浏览器异常或缺失值当成成功。 */
async function evaluate(expression) {
    const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
    return result.result?.value;
}

/** 等待组件异步渲染和轮询，最多十秒。 */
async function waitFor(expression) {
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline) {
        if (await evaluate(expression)) {
            return;
        }
        await new Promise((resolveWait) => setTimeout(resolveWait, 25));
    }
    throw new Error(`DOM condition timed out: ${expression}`);
}

/** 通过用户实际可见的确认框和主按钮执行一步，不直接调用组件内部函数。 */
async function clickMain() {
    await waitFor(`!!document.querySelector('input[type=checkbox]')`);
    await evaluate(`if(!document.querySelector('input').checked)document.querySelector('input').click()`);
    await waitFor(`!document.querySelector('button.btn-dark').disabled`);
    await evaluate(`document.querySelector('button.btn-dark').click()`);
}

/** 保存当前组件的桌面或手机截图，供人工检查布局。 */
async function screenshot(name) {
    if (output) {
        await mkdir(output, { recursive: true });
        const result = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true });
        await writeFile(resolve(output, name), Buffer.from(result.data, 'base64'));
    }
}

try {
    await send('Page.enable');
    await send('Runtime.enable');
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 1000, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: `http://127.0.0.1:${server.address().port}/` });
    await waitFor(`!!document.querySelector('button.btn-dark')`);
    assert.equal(await evaluate(`document.querySelector('button.btn-dark').disabled`), true);
    await clickMain();
    await waitFor(`window.phase.active===true && document.querySelector('h4')?.textContent.includes('摆正')`);
    for (let step = 0; step < 10; step++) {
        if (step === 4) {
            await screenshot('head-calibration-desktop.png');
            await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
            await screenshot('head-calibration-mobile.png');
            assert(await evaluate(`document.documentElement.scrollWidth<=innerWidth+1`));
            await evaluate('window.failNext=true');
            await clickMain();
            await waitFor(`document.querySelector('section').textContent.includes('补偿误差过大')`);
            assert.equal(await evaluate('window.phase.next_step'), 4);
            assert.equal(await evaluate(`window.calls.filter(c=>c.args.op==='head-save').length`), 0);
        }
        await clickMain();
        await waitFor(`window.phase.next_step===${step + 1} && document.querySelector('p.num')?.textContent.includes('${step + 1} / 10')`);
    }
    assert.equal(await evaluate(`window.calls.filter(c=>c.args.op==='head-save').length`), 0);
    await clickMain();
    await waitFor(`document.querySelector('section').textContent.includes('已保存头部安装核验')`);
    const calls = await evaluate('window.calls');
    assert.equal(calls.filter((call) => call.args.op === 'head-save').length, 1);
    assert(calls.every((call) => call.name === 'imu.op' && Object.keys(call.args).every(
        (key) => ['op', 'step', 'confirmed'].includes(key))));
    await evaluate('window.unmount()');
    const before = await evaluate('window.calls.length');
    await new Promise((resolveWait) => setTimeout(resolveWait, 1200));
    assert.equal(await evaluate('window.calls.length'), before, 'unmount must stop polling');
    assert.deepEqual(errors, []);
    process.stdout.write(JSON.stringify({ passed: true, captures: 11, explicitSaves: 1,
        failureDidNotAdvance: true, stoppedPolling: true, browserErrors: errors }) + '\n');
} finally {
    await send('Page.close').catch(() => {});
    socket.close();
    server.close();
}
