import { html, useEffect, useRef, useState } from './vendor/preact-htm.module.js';

export const HEAD_CALIBRATION_STEPS = [
    ['站姿静止', '鸭子通电站在平桌上，保持已有站姿，静止约 3 秒。不是躺在桌上。'],
    ['整机前倾', '双手握躯干稍微提起，先端平，再让头那一侧向下倾约 15–20°，停稳约 2 秒；头颈由舵机保持。'],
    ['整机后倾', '先将整只鸭子回到站姿端平、停半秒，再向后倾约 15–20°，停稳约 2 秒。'],
    ['整机左倾', '先回到站姿端平、停半秒，再向鸭子自己的左侧倾约 15–20°，停稳约 2 秒。'],
    ['整机右倾', '先回到站姿端平、停半秒，再向鸭子自己的右侧倾约 15–20°，停稳约 2 秒。'],
    ['桌面站姿基准', '放回桌面，保持通电站立；机身站稳后自动采集，不需要扶头或掰关节。'],
    ['ID30 颈俯仰', '程序自动缓慢测试颈俯仰约 ±5°，在中位、两侧和回中位分别停稳采集，途中允许卡顿。'],
    ['ID31 头俯仰', '程序自动缓慢测试头俯仰约 ±5°，在中位、两侧和回中位分别停稳采集，途中允许卡顿。'],
    ['ID32 头偏航（已跳过）', '偏航不参与安装与补偿核验：主控按当前设置跳过本步，不产生测量，也不把跳过当成通过。'],
    ['ID33 头横滚', '程序自动缓慢测试头横滚约 ±5°，在中位、两侧和回中位分别停稳采集，途中允许卡顿。'],
];

/** 将原生阶段映射为明确的用户操作；自动采集期间不提供逐步采集按钮。 */
function mainAction(state) {
    if (!state?.active) {
        return { operation: 'start', label: '开始整机站姿采集' };
    }
    if (state.ready_to_save) {
        return { operation: 'save', label: '保存安装与补偿核验结果' };
    }
    if (state.next_step >= 5) {
        return { operation: 'test', label: '开始 / 重试自动头部测试（偏航已跳过）' };
    }
    return { operation: 'resume', label: '继续整机站姿采集' };
}

/** 通电站姿两阶段向导；原生主控采样和调度，查询只续租，不自动恢复暂停的轨迹。 */
export function HeadCalibrationPanel({ imu, link, live, bodyCapture = null, onState, onStart }) {
    const [state, setState] = useState(null);
    const [message, setMessage] = useState('');
    const [confirmation, setConfirmation] = useState(null);
    const [busy, setBusy] = useState(false);
    const [fetching, setFetching] = useState(false);
    const pending = useRef(false);
    const commanding = useRef(false);
    const revision = useRef(0);
    const alive = useRef(true);
    const phase = state?.next_step >= 5 ? 'head-test' : 'body-capture';
    const action = mainAction(state);
    const confirmKey = `${imu?.mode}/${phase}/${action.operation}`;
    const confirmed = confirmation === confirmKey;
    const holding = imu?.mode === 'commissioning' && imu?.hold?.stage === 'holding';
    const active = state?.active === true;
    const running = state?.running === true;
    const step = state?.next_step ?? 0;
    const saved = !!imu?.head_verification || state?.saved === true;
    const reports = (state?.reports || []).filter((report) => report.step >= 6);

    useEffect(() => {
        onState?.(state);
    }, [state, onState]);

    useEffect(() => {
        alive.current = true;
        let timer;
        let stopped = false;
        /** 前台每秒查询会话；离页停止续租，由主控在 2.5 秒内停止轨迹并保留站姿。 */
        async function poll() {
            if (!document.hidden && !pending.current && !commanding.current && live) {
                pending.current = true;
                setFetching(true);
                const requestRevision = revision.current;
                try {
                    const reply = await link.cmd('imu.op', { op: 'head-status' }, 10000);
                    if (!stopped && requestRevision === revision.current) {
                        if (reply.ok) {
                            setState(reply.data);
                        } else {
                            setMessage(reply.reason || reply.error || '无法读取站姿核验状态');
                        }
                    }
                } catch {
                    if (!stopped) {
                        setMessage('连接中断；主控将停止测试轨迹，恢复连接后需明确重试。');
                    }
                } finally {
                    pending.current = false;
                    if (!stopped) {
                        setFetching(false);
                    }
                }
            }
            if (!stopped) {
                timer = setTimeout(poll, 1000);
            }
        }
        poll();
        return () => {
            stopped = true;
            alive.current = false;
            clearTimeout(timer);
        };
    }, [link, live]);

    /** 明确启动某阶段或停止；失败保留原生进度，不在页面推测成功。 */
    async function run(operation) {
        const stopping = operation === 'pause' || operation === 'cancel';
        if (commanding.current || !live || (!stopping && (pending.current || !confirmed))) {
            return;
        }
        if (operation === 'start' || operation === 'resume') {
            onStart?.();
        }
        commanding.current = true;
        revision.current += 1;
        setBusy(true);
        setMessage('');
        try {
            const reply = await link.cmd('imu.op', { op: `head-${operation}`, confirmed: true }, 10000);
            if (!alive.current) {
                return;
            }
            if (reply.ok) {
                setState(reply.data);
                setConfirmation(null);
                if (reply.data?.saved) {
                    setMessage('已保存头部安装核验。当前仍通电保持站姿；扶稳后卸力，再回上手清单预览并写进主程序。');
                }
            } else {
                setMessage(reply.reason || reply.error || '操作未完成，请检查主控返回的原因。');
            }
        } catch {
            if (alive.current) {
                setMessage('未收到主控确认，请恢复连接查看实际状态；不会自动重试动作。');
            }
        } finally {
            commanding.current = false;
            if (alive.current) {
                setBusy(false);
            }
        }
    }

    let confirmText = '鸭子已通电保持站姿，我将双手握躯干按提示倾斜整机；不需要扶头';
    if (action.operation === 'test') {
        confirmText = '鸭子已放回桌面并站稳，我已确认头颈约 ±5° 内没有干涉，同意程序自动测试四个关节';
    } else if (action.operation === 'save') {
        confirmText = '我确认保存这次真实采集和自动测试的核验结果';
    }
    return html`<section class="board imu-card head-calibration" aria-labelledby="head-calibration-title">
        <h3 id="head-calibration-title">头部 BMI088 · 通电站姿核验</h3>
        ${bodyCapture}
        <p>先用上方“站姿使能”。第一段保持站姿、双手握躯干提起并倾斜整机；第二段放回桌面站立，由程序自动测试头颈。</p>
        <p class="sv-small">全程保留原有机械零位。头颈由舵机保持，不需要手扶头部或手动掰通电关节。
            “停止测试”保留通电站姿；上方“失能 / 卸力”会让整只鸭子软下来。</p>
        ${saved && !active ? html`<p role="status">已读到主控保存的头部核验结果。扶稳并卸力后，可回上手清单继续。</p>` : null}
        ${active && step >= 5 ? html`<p class="sv-small">ID32 偏航已按设置跳过，仅作诊断位保留，不作为安装依据。</p>
            <p class="num">头部阶段 ${Math.min(step - 5, 5)} / 5</p>
            ${HEAD_CALIBRATION_STEPS[step] ? html`<h4>${HEAD_CALIBRATION_STEPS[step][0]}</h4>
                <p>${HEAD_CALIBRATION_STEPS[step][1]}</p>` : null}
            ${state.countdown_s > 0 && running ? html`<p role="status">准备时间：${state.countdown_s} 秒</p>` : null}
            <p role="status">${state.message}</p>
            <ol>${HEAD_CALIBRATION_STEPS.slice(5).map(([title], offset) => {
                const index = offset + 5;
                return html`<li key=${index} aria-current=${index === step ? 'step' : null}>
                    ${index < step ? '✓ ' : ''}${title}</li>`;
            })}</ol>` : null}
        ${active && step < 5 ? html`<p role="status">${state.message}</p>` : null}
        ${step >= 6 ? html`<p class="sv-small">端点停稳后采集；正向、反向分别核对累计旋转。
            途中卡顿的瞬时角速度仅作诊断，不要求匀速。当前端点：${state?.test_point ?? '等待开始'} / 3。</p>` : null}
        ${reports.length ? html`<details><summary>已完成的头部测量</summary>
            <table><thead><tr><th>关节</th><th>端点姿态差</th><th>单向积分误差</th><th>实测行程</th></tr></thead>
                <tbody>${reports.map((report) => html`<tr><td>ID${report.step + 24}</td>
                    ${report.skipped ? html`<td colspan="3">已跳过（未测，不作安装依据）</td>` : html`
                    <td>${Number.isFinite(report.max_body_error_deg) ? report.max_body_error_deg.toFixed(2) + '°' : '未测'}</td>
                    <td>${Number.isFinite(report.gyro_integral_error_ratio) ? (report.gyro_integral_error_ratio * 100).toFixed(1) + '%' : '未测'}</td>
                    <td>${Number.isFinite(report.head_span_deg) ? report.head_span_deg.toFixed(1) + '°' : '未测'}</td>`}</tr>`)}</tbody>
            </table></details>` : null}
        ${!running ? html`<label><input type="checkbox" checked=${confirmed} disabled=${busy}
            onChange=${(event) => setConfirmation(event.currentTarget.checked ? confirmKey : null)} />${confirmText}</label>` : null}
        <div class="imu-actions">
            ${!running ? html`<button class="btn-dark" disabled=${busy || fetching || !confirmed || !live || !holding}
                onClick=${() => run(action.operation)}>${busy ? '等待主控确认…' : action.label}</button>` : null}
            ${active && running ? html`<button class="btn-dark" disabled=${busy || !live}
                onClick=${() => run('pause')}>停止采集 / 测试，保留站姿</button>` : null}
            ${active ? html`<button disabled=${busy || !live} onClick=${() => run('cancel')}>取消本次核验，保留站姿</button>` : null}
        </div>
        ${!holding ? html`<p class="sv-warn">需要主控在线、处于标定模式并完成站姿保持。先点上方“站姿使能”。</p>` : null}
        ${message ? html`<p class="sv-warn" role="status">${message}</p>` : null}
    </section>`;
}
