/*
 * SectorFlux - LLM Proxy and Analytics
 * Copyright (c) 2025 ParticleSector.com
 *
 * This software is dual-licensed:
 * - GPL-3.0 for open source use
 * - Commercial license for proprietary use
 *
 * See LICENSE and LICENSING.md for details.
 */

const API = {
    async fetchMetrics() {
        const response = await fetch('/api/metrics');
        return response.json();
    },

    async fetchLogs() {
        const response = await fetch('/api/logs');
        return response.json();
    },

    async fetchLog(id) {
        const response = await fetch(`/api/logs/${id}`);
        if (!response.ok) throw new Error('Log not found');
        return response.json();
    },

    async replayRequest(id) {
        const response = await fetch(`/api/replay/${id}`, {
            method: 'POST',
            headers: {
                'X-SectorFlux-No-Cache': 'true'
            }
        });
        return response.ok;
    },

    async shutdown() {
        return fetch('/api/shutdown', { method: 'POST' });
    },

    async getCacheConfig() {
        const response = await fetch('/api/config/cache');
        return response.json();
    },

    async setCacheConfig(enabled) {
        const response = await fetch('/api/config/cache', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ enabled: enabled })
        });
        if (!response.ok) throw new Error('Failed to update cache configuration');
        return response;
    },

    async fetchModels() {
        const response = await fetch('/api/tags');
        return response.json();
    },

    async setLogStarred(id, starred) {
        const response = await fetch(`/api/logs/${id}/starred`, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ starred })
        });
        if (!response.ok) throw new Error('Failed to update starred status');
        return response.json();
    },

    async fetchVersion() {
        const response = await fetch('/api/version');
        return response.json();
    }
};
