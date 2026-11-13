// 原始 HAT 数据显示；不发送电机命令，不写安装标定。
import { html, useState, useEffect } from './vendor/preact-htm.module.js';
import { Quaternion, Euler, Vector3 } from './vendor/three/three.module.js';

const DEG_PER_RAD = 180 / Math.PI;
const VIEW_ROTATION = new Euler(-1.0, 0, -0.55, 'XYZ');

/** 将服务的 w,x,y,z 数组转为 Three.js 的归一化 x,y,z,w 四元数。 */
export function sensorQuaternion(values) {
    if (!Array.isArray(values) || values.length !== 4 || !values.every(Number.isFinite)) {
        throw new Error('IMU 姿态数据无效');
    }
    const [w, x, y, z] = values;
    const quaternion = new Quaternion(x, y, z, w);
    if (quaternion.length() < 0.9 || quaternion.length() > 1.1) {
        throw new Error('IMU 姿态长度无效');
    }
    return quaternion.normalize();
}

/** 按可选显示参考绘制传感器姿态，不持久化、不改变物理安装方向。 */
export function displayAttitude(values, reference) {
    const quaternion = sensorQuaternion(values);
    if (reference) {
        quaternion.premultiply(sensorQuaternion(reference).invert());
    }
    // ZYX convention: yaw about Z, pitch about Y, roll about X. Near ±90° pitch,
    // Euler angles are ambiguous, but the quaternion-driven drawing remains continuous.
    const euler = new Euler().setFromQuaternion(quaternion, 'ZYX');
    return { quaternion, roll: euler.x * DEG_PER_RAD,
        pitch: euler.y * DEG_PER_RAD, yaw: euler.z * DEG_PER_RAD };
}

/** 有效零值显示为零，缺失读数显示为短横线。 */
function reading(value, digits = 1) {
    if (!Number.isFinite(value)) {
        return '—';
    }
    return (Math.abs(value) < 0.5 * 10 ** -digits ? 0 : value).toFixed(digits);
}

/** 页面可见时轮询；即使网络未完成，旧样本也会按年龄失效。 */
function useHeadImu() {
    const [sample, setSample] = useState(null);
    const [error, setError] = useState('');
    const [clock, setClock] = useState(performance.now());
    const [paused, setPaused] = useState(document.hidden);
    useEffect(() => {
        let stopped = false;
        let timer;
        let controller;
        let pending = false;
        /** 限时读取一个已登录快照，避免轮询请求重叠。 */
        async function poll() {
            if (stopped || pending || document.hidden) {
                return;
            }
            pending = true;
            const started = performance.now();
            controller = new AbortController();
            const deadline = setTimeout(() => controller.abort(), 1500);
            try {
                const response = await fetch('/api/head-imu', {
                    credentials: 'same-origin', cache: 'no-store', signal: controller.signal,
                });
                const result = await response.json();
                if (!response.ok || !result.ok) {
                    throw new Error(response.status === 401 ? '请重新登录后查看 IMU' : result.error || '无法读取 HAT IMU');
                }
                if (!result.data || typeof result.data.fresh !== 'boolean') {
                    throw new Error('IMU 响应格式不正确');
                }
                if (result.data.fresh) {
                    sensorQuaternion(result.data.frame?.quat);
                }
                if (!stopped) {
                    // Count request time conservatively: a delayed response cannot revive old data.
                    setSample({ ...result.data, received: performance.now(), requestMs: performance.now() - started });
                    setError('');
                }
            } catch (failure) {
                if (!stopped && !document.hidden) {
                    setError(failure.name === 'AbortError' ? '连接超时，正在重试' : failure.message);
                }
            } finally {
                clearTimeout(deadline);
                pending = false;
                if (!stopped && !document.hidden) {
                    timer = setTimeout(poll, 100);
                }
            }
        }
        /** 页面隐藏时取消请求，用户返回后恢复轮询。 */
        function visibilityChanged() {
            setPaused(document.hidden);
            clearTimeout(timer);
            if (document.hidden) {
                controller?.abort();
            } else {
                poll();
            }
        }
        const ticker = setInterval(() => setClock(performance.now()), 200);
        document.addEventListener('visibilitychange', visibilityChanged);
        poll();
        return () => {
            stopped = true;
            clearTimeout(timer);
            clearInterval(ticker);
            controller?.abort();
            document.removeEventListener('visibilitychange', visibilityChanged);
        };
    }, []);
    const age = sample && Number.isFinite(sample.age_ms)
        ? sample.age_ms + sample.requestMs + Math.max(0, clock - sample.received) : null;
    return { sample, error, paused, age,
        fresh: !paused && !error && sample?.fresh === true && age !== null && age < 800 };
}

/** 按四元数绘制传感器坐标示意，不把示意几何当作实装方向。 */
function SensorPose({ attitude }) {
    if (!attitude) {
        return html`<div class="hat-pose-empty">等待有效姿态数据</div>`;
    }
    /** 将传感器空间中的点投影到固定 SVG 观察方向。 */
    function project(coordinates) {
        const point = new Vector3(...coordinates).applyQuaternion(attitude.quaternion).applyEuler(VIEW_ROTATION);
        return { x: 190 + point.x * 90, y: 145 - point.y * 90, z: point.z };
    }
    const corners = [[-0.95,-0.6,-0.08],[0.95,-0.6,-0.08],[0.95,0.6,-0.08],[-0.95,0.6,-0.08],
        [-0.95,-0.6,0.08],[0.95,-0.6,0.08],[0.95,0.6,0.08],[-0.95,0.6,0.08]].map(project);
    const faces = [[0,1,2,3],[4,5,6,7],[0,1,5,4],[1,2,6,5],[2,3,7,6],[3,0,4,7]]
        .map((indices, index) => ({ indices, index, depth: indices.reduce((sum, item) => sum + corners[item].z, 0) / 4 }))
        .sort((first, second) => first.depth - second.depth);
    const origin = project([0,0,0]);
    const axes = [['X',[1.45,0,0]],['Y',[0,1.4,0]],['Z',[0,0,1.35]]];
    return html`<svg viewBox="0 0 380 300" role="img" aria-label="HAT IMU 三维传感器姿态示意">
        <path d="M45 235H335 M190 222V248" stroke="#484a50" fill="none" />
        ${faces.map((face) => html`<polygon key=${face.index}
            points=${face.indices.map((index) => `${corners[index].x},${corners[index].y}`).join(' ')}
            fill=${face.index === 1 ? '#5662c5' : '#32343b'} stroke="#a4adff" stroke-width="1.3" />`)}
        ${axes.map(([name, coordinates]) => {
            const end = project(coordinates);
            return html`<g key=${name}><line x1=${origin.x} y1=${origin.y} x2=${end.x} y2=${end.y}
                stroke="#eeeeF2" stroke-width="2" /><circle cx=${end.x} cy=${end.y} r="3" fill="#ffffff" />
                <text x=${end.x + 9} y=${end.y - 9} fill="#ffffff" font-size="15">${name}</text></g>`;
        })}
        <text x="190" y="279" text-anchor="middle" fill="#c7c9cf" font-size="12">传感器坐标示意 · 非主板安装方向</text>
    </svg>`;
}

/** 展示独立 HAT 读数，不触发躯干标定或运动。 */
export function HeadImuPanel() {
    const { sample, error, paused, age, fresh } = useHeadImu();
    const [reference, setReference] = useState(null);
    const frame = fresh ? sample.frame : null;
    const activeReference = reference?.session === sample?.session ? reference : null;
    const attitude = frame ? displayAttitude(frame.quat, activeReference?.quat) : null;
    let state = '等待数据';
    if (fresh) {
        state = '实时读取中';
    } else if (paused) {
        state = '页面已暂停';
    } else if (error) {
        state = '连接异常';
    }
    /** 仅设置浏览器内的姿态显示参考，不写系统设置。 */
    function setDisplayReference() {
        if (fresh) {
            setReference({ session: sample.session, quat: [...frame.quat] });
        }
    }
    /** 恢复服务提供的传感器坐标显示，不影响硬件标定。 */
    function clearDisplayReference() {
        setReference(null);
    }
    return html`<section class="hat-imu" aria-labelledby="hat-imu-title">
        <header class="hat-imu-heading"><div><h2 id="hat-imu-title">HAT 板载 BMI088</h2>
            <p>头部传感器原始读数 · 不等于躯干姿态</p></div>
            <span class=${'hat-status ' + (fresh ? 'live' : 'waiting')} role="status">${state}</span></header>
        <div class="hat-imu-grid">
            <div class="hat-pose"><${SensorPose} attitude=${attitude} /></div>
            <div class="hat-readings">
                <div class="hat-angles num">
                    ${[['横滚 X',attitude?.roll],['俯仰 Y',attitude?.pitch],['偏航 Z',attitude?.yaw]].map(([label,value]) =>
                        html`<div key=${label}><span>${label}</span><b>${reading(value)}<small>°</small></b></div>`)}
                </div>
                <p class="hat-frame-note">${activeReference ? '相对当前显示参考' : '传感器原始姿态'} · 偏航会漂移，不代表正北</p>
                <table class="hat-vectors num"><thead><tr><th scope="col">原始传感器轴</th><th scope="col">X</th><th scope="col">Y</th><th scope="col">Z</th></tr></thead>
                    <tbody><tr><th scope="row">加速度 <small>m/s²</small></th>
                        ${[0,1,2].map((index) => html`<td key=${index}>${reading(frame?.accel?.[index], 2)}</td>`)}</tr>
                    <tr><th scope="row">角速度 <small>°/s</small></th>
                        ${[0,1,2].map((index) => html`<td key=${index}>${reading(frame?.gyro?.[index] == null ? null : frame.gyro[index] * DEG_PER_RAD, 2)}</td>`)}</tr></tbody></table>
                <div class="hat-meta num"><span>温度 <b>${reading(frame?.temp_c)} °C</b></span>
                    <span>采样 <b>${reading(fresh ? sample.measured_hz : null)} Hz</b></span>
                    <span>帧号 <b>${frame?.seq ?? '—'}</b></span></div>
                <div class="hat-actions"><button type="button" disabled=${!fresh} onClick=${setDisplayReference}>当前姿态设为显示参考</button>
                    ${activeReference ? html`<button type="button" onClick=${clearDisplayReference}>恢复原始姿态</button>` : null}</div>
                <p class="hat-help">显示参考仅在此页面生效，不写入机械零位或躯干标定。</p>
            </div>
        </div>
        <footer class="hat-feedback" role="status">${fresh
            ? `实时数据 · 约 ${Math.round(age)} ms 前更新 · 轻轻转动板子观察变化`
            : error || sample?.reason || '正在订阅 HAT 数据，请稍候…'}</footer>
    </section>`;
}
