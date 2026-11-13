import { html, useEffect, useRef, useState } from './vendor/preact-htm.module.js';

export const HEAD_CALIBRATION_STEPS = [
    ['摆正，采初始姿态', '托住整只鸭子，使躯干水平；承托头颈并保持不变，停稳至少 2 秒。'],
    ['机身前倾，求安装方向', '头颈相对机身保持上一步姿态；让整个机身前倾 15–30°，停稳至少 2 秒。不是只低头。'],
    ['机身左倾，求安装方向', '头颈仍保持初始姿态；让整个机身向鸭子自己的左侧倾斜 15–30°，停稳至少 2 秒。'],
    ['摆正，建立独立复核基准', '将整个机身重新摆正、固定好躯干；头部保持不动，停稳至少 2 秒。'],
    ['固定躯干，核对 ID30 颈俯仰', '固定躯干，只手动缓慢转动 ID30，往返行程至少 8°；其他头颈关节保持不变。连续操作约 3 秒后立即采集。'],
    ['固定躯干，核对 ID31 头俯仰', '固定躯干，只手动缓慢转动 ID31，往返行程至少 8°；其他头颈关节保持不变，操作后立即采集。'],
    ['固定躯干，核对 ID32 头偏航', '固定躯干，只手动缓慢转动 ID32，往返行程至少 8°；其他头颈关节保持不变，操作后立即采集。'],
    ['固定躯干，核对 ID33 头横滚', '固定躯干，只手动缓慢转动 ID33，往返行程至少 8°；保持在机械允许范围内，其他关节不动，操作后立即采集。'],
    ['机身前倾，复核补偿方向', '固定头颈相对机身，整个机身前倾 15–30°，停稳至少 2 秒；确认主程序读到的是身体向前倾斜。'],
    ['机身左倾，复核补偿方向', '固定头颈相对机身，整个机身向鸭子自己的左侧倾斜 15–30°，停稳至少 2 秒。'],
];

/** 独立头部向导只发送步骤操作；测量、误差判定和保存都由主程序完成。 */
export function HeadCalibrationPanel({ imu, link, live }) {
    const [state, setState] = useState(null);
    const [message, setMessage] = useState('');
    const [confirmation, setConfirmation] = useState(null);
    const stepKey = `${imu?.mode}/${state?.active === true ? state.next_step : 'start'}`;
    const confirmed = confirmation === stepKey;
    const [busy, setBusy] = useState(false);
    const [fetching, setFetching] = useState(false);
    const pending = useRef(false);
    const alive = useRef(true);
    const statusError = useRef(false);
    useEffect(() => {
        alive.current = true;
        let timer;
        let stopped = false;
        /** 前台有界查询主控会话；不会因为轮询而采集、保存或改变运动模式。 */
        async function poll() {
            if (!document.hidden && !pending.current && live) {
                pending.current = true;
                setFetching(true);
                try {
                    const reply = await link.cmd('imu.op', { op: 'head-status' }, 10000);
                    if (!stopped) {
                        if (reply.ok) {
                            setState(reply.data);
                            statusError.current = false;
                        } else if (!statusError.current) {
                            setMessage(reply.reason || reply.error || '无法读取头部核验状态');
                            statusError.current = true;
                        }
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
    /** 用户明确确认后提交当前步骤；失败保留原步骤及具体拒绝原因。 */
    async function run(operation) {
        if (pending.current || busy || !live || (!confirmed && operation !== 'cancel')) {
            return;
        }
        pending.current = true;
        setBusy(true);
        setMessage('');
        try {
            const reply = await link.cmd('imu.op', {
                op: `head-${operation}`, step: state?.next_step ?? 0, confirmed: true,
            }, 10000);
            if (!alive.current) {
                return;
            }
            if (reply.ok) {
                setState(reply.data);
                setConfirmation(null);
                setMessage(reply.data?.saved
                    ? '主程序已保存头部安装核验。请回总览“上手清单”，先预览，再写进主程序。'
                    : operation === 'cancel' ? '会话已取消，本次未保存安装方向。' : '这一步通过，请按下一步提示继续。');
            } else {
                setMessage(reply.reason || reply.error || '未通过；请按原因调整后重试，本次未保存。');
            }
        } finally {
            pending.current = false;
            if (alive.current) {
                setBusy(false);
            }
        }
    }
    const active = state?.active === true;
    const step = state?.next_step ?? 0;
    const ready = active && state?.ready_to_save === true;
    const saved = !!imu?.head_verification || state?.saved === true;
    const current = HEAD_CALIBRATION_STEPS[step];
    const canStart = live && imu?.mode === 'commissioning';
    const reports = state?.reports || [];
    return html`<section class="board imu-card head-calibration" aria-labelledby="head-calibration-title">
        <h3 id="head-calibration-title">头部 BMI088 专用安装核验</h3>
        <p>此传感器随头部运动。需要同时校验安装方向和实际关节补偿，不能使用固定躯干 IMU 的粗校流程。</p>
        <p class="sv-small">全程承托躯干和头部、保持舵机断力，手动操作。不会自动转动关节；不要让头部自由垂落或碰到机械限位。
            本次保存只更新头部 IMU 核验，保留已完成的舵机零位。</p>
        ${saved && !active ? html`<p role="status">已读到主控保存的头部核验结果。可回上手清单继续“写进主程序”。</p>` : null}
        ${active ? html`<p class="num">进度 ${Math.min(step, 10)} / 10 · 当前窗口 ${state.sample_count ?? 0} 帧</p>
            ${current ? html`<h4>${step + 1}. ${current[0]}</h4><p>${current[1]}</p>`
                : html`<p>十步实测已齐全。保持连接和断力，确认保存本次安装核验。</p>`}
            <ol>${HEAD_CALIBRATION_STEPS.map(([title], index) => html`<li key=${index}
                aria-current=${index === step ? 'step' : null}>${index < step ? '✓ ' : ''}${title}</li>`)}</ol>`
            : html`<p>准备好后开始：摆正、整机前倾、整机左倾求安装；再逐个转动四个头颈关节，并复核整机倾斜。</p>`}
        <label><input type="checkbox" checked=${confirmed} disabled=${busy}
            onChange=${(event) => setConfirmation(event.currentTarget.checked ? stepKey : null)} />
            ${active && step >= 4 && step <= 7 ? '我已固定躯干，只缓慢转动当前指定关节'
                : ready ? '我确认保存本次真实采样结果，鸭子仍已承托且断力'
                    : '我已按当前提示承托并固定机身或头部，舵机已断力'}</label>
        <div class="imu-actions">
            <button class="btn-dark" disabled=${busy || fetching || !confirmed || !canStart}
                onClick=${() => run(active ? ready ? 'save' : 'capture' : 'start')}>
                ${busy ? '正在核验…' : active ? ready ? '保存头部核验结果' : '采集并核对这一步' : '开始头部专用核验'}</button>
            ${active ? html`<button disabled=${busy || fetching || !live} onClick=${() => run('cancel')}>取消本次核验</button>` : null}
        </div>
        ${!canStart ? html`<p class="sv-warn">需要主程序在线并处于标定模式。当前不可开始新的头部安装核验。</p>` : null}
        ${reports.length > 0 ? html`<details><summary>已通过步骤的实测结果</summary><ul>
            ${reports.map((report) => html`<li key=${report.step}>步骤 ${report.step + 1}：${report.sample_count} 帧
                ${report.step >= 4 && report.step <= 7
                    ? `，躯干姿态残差 ${report.max_body_error_deg.toFixed(2)}°，角速度残差 ${report.body_gyro_rms_rad_s.toFixed(3)} rad/s`
                    : ''}</li>`)}</ul></details>` : null}
        ${message ? html`<p class="sv-warn" role="status">${message}</p>` : null}
    </section>`;
}
