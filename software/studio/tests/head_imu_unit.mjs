import assert from 'node:assert/strict';
import { test } from 'node:test';
import { displayAttitude, sensorQuaternion } from '../web/head-imu.js';

test('HAT display respects wxyz axes and a display-only reference', () => {
    const halfAngleRad = Math.PI / 4;
    const roll = [Math.cos(halfAngleRad), Math.sin(halfAngleRad), 0, 0];
    const yaw = [Math.cos(halfAngleRad), 0, 0, Math.sin(halfAngleRad)];
    assert.ok(Math.abs(displayAttitude(roll).roll - 90) < 1e-9);
    assert.ok(Math.abs(displayAttitude(yaw).yaw - 90) < 1e-9);
    assert.ok(Math.abs(displayAttitude(yaw, yaw).yaw) < 1e-9);
    assert.deepEqual(yaw, [Math.cos(halfAngleRad), 0, 0, Math.sin(halfAngleRad)]);
});

test('invalid or missing HAT quaternions never become upright', () => {
    for (const value of [null, [], [0, 0, 0, 0], [true, 0, 0, 0], [NaN, 0, 0, 0], [2, 0, 0, 0]]) {
        assert.throws(() => sensorQuaternion(value));
    }
    assert.equal(sensorQuaternion([1.01, 0, 0, 0]).length(), 1);
});
