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

document.addEventListener('DOMContentLoaded', () => {
    const logsTableBody = document.querySelector('#logsTable tbody');
    const modal = document.getElementById('details-modal');
    const modalBody = document.getElementById('modal-body');
    const closeBtn = document.querySelector('.close');

    // Metrics Elements
    const metricTotal = document.getElementById('metric-total');
    const metricLatency = document.getElementById('metric-latency');
    const metricCache = document.getElementById('metric-cache');

    async function fetchMetrics() {
        try {
            const data = await API.fetchMetrics();
            metricTotal.textContent = data.total_requests;
            metricLatency.textContent = `${data.avg_latency_ms.toFixed(0)}ms`;
            metricCache.textContent = `${(data.cache_hit_rate * 100).toFixed(1)}%`;
        } catch (error) {
            console.error('Error fetching metrics:', error);
        }
    }

    async function fetchLogs() {
        try {
            const data = await API.fetchLogs();
            renderLogs(data);
            fetchMetrics(); // Update metrics when logs update
        } catch (error) {
            console.error('Error fetching logs:', error);
        }
    }

    function renderLogs(logs) {
        logsTableBody.innerHTML = '';
        logs.forEach(log => {
            const row = document.createElement('tr');
            const duration = `${log.duration_ms}ms`;

            // Calculate Metrics
            let tps = '-';
            let ttft = '-';

            if (log.eval_duration_ms > 0 && log.completion_tokens > 0) {
                const tpsVal = log.completion_tokens / (log.eval_duration_ms / 1000);
                tps = `${tpsVal.toFixed(1)} t/s`;
            }

            if (log.prompt_eval_duration_ms > 0) {
                ttft = `${log.prompt_eval_duration_ms}ms`;
            }

            const starIcon = log.is_starred ? '★' : '☆';
            const starClass = log.is_starred ? 'star-btn starred' : 'star-btn';

            row.innerHTML = `
                <td><button class="${starClass}" data-id="${log.id}" data-starred="${log.is_starred}">${starIcon}</button></td>
                <td>${log.id}</td>
                <td>${new Date(log.timestamp.replace(' ', 'T') + 'Z').toLocaleTimeString()}</td>
                <td><span class="model-tag">${log.model || '-'}</span></td>
                <td>${log.prompt_tokens || '-'}</td>
                <td>${log.completion_tokens || '-'}</td>
                <td>${duration}</td>
                <td>${ttft}</td>
                <td>${tps}</td>
                <td>
                    <button class="btn-sm view-btn" data-id="${log.id}">View</button>
                    <button class="btn-sm replay-btn" data-id="${log.id}">Replay</button>
                </td>
            `;
            logsTableBody.appendChild(row);
        });

        // Attach event listeners for star buttons
        document.querySelectorAll('.star-btn').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const id = parseInt(e.target.getAttribute('data-id'));
                const currentlyStarred = e.target.getAttribute('data-starred') === 'true';
                const newStarred = !currentlyStarred;

                // Optimistic UI update
                e.target.textContent = newStarred ? '★' : '☆';
                e.target.className = newStarred ? 'star-btn starred' : 'star-btn';
                e.target.setAttribute('data-starred', newStarred);

                try {
                    await API.setLogStarred(id, newStarred);
                } catch (error) {
                    console.error('Error updating starred status:', error);
                    // Revert on error
                    e.target.textContent = currentlyStarred ? '★' : '☆';
                    e.target.className = currentlyStarred ? 'star-btn starred' : 'star-btn';
                    e.target.setAttribute('data-starred', currentlyStarred);
                }
            });
        });

        // Attach event listeners for view buttons
        document.querySelectorAll('.view-btn').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const id = parseInt(e.target.getAttribute('data-id'));
                try {
                    const log = await API.fetchLog(id);
                    showDetails(log);
                } catch (error) {
                    console.error('Error fetching log details:', error);
                }
            });
        });

        document.querySelectorAll('.replay-btn').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const id = parseInt(e.target.getAttribute('data-id'));
                replayRequest(id, e.target);
            });
        });
    }

    function showDetails(log) {
        let reqBody = log.request_body;
        let resBody = log.response_body;

        try { reqBody = JSON.stringify(JSON.parse(reqBody), null, 2); } catch (e) { }

        modalBody.innerHTML = `
            <h3>Request</h3>
            <pre>${escapeHtml(reqBody)}</pre>
            <h3>Response</h3>
            <pre>${escapeHtml(resBody)}</pre>
        `;
        modal.style.display = 'block';
    }

    async function replayRequest(id, button) {
        // Show loading state
        button.disabled = true;
        button.textContent = 'Replaying...';
        showToast(`Replaying request #${id}...`, 'info');

        try {
            const success = await API.replayRequest(id);
            if (success) {
                showToast('Replay complete!', 'success');
                setTimeout(fetchLogs, 500);
            } else {
                showToast('Replay failed', 'error');
            }
        } catch (error) {
            console.error('Error replaying:', error);
            showToast('Replay error', 'error');
        } finally {
            // Restore button state
            button.disabled = false;
            button.textContent = 'Replay';
        }
    }

    function escapeHtml(text) {
        if (!text) return '';
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;");
    }

    // Toast Notifications
    const toastContainer = document.getElementById('toast-container');

    function showToast(message, type = 'info') {
        const toast = document.createElement('div');
        toast.className = `toast ${type}`;
        toast.textContent = message;
        toastContainer.appendChild(toast);

        // Auto-dismiss after 3 seconds
        setTimeout(() => {
            toast.classList.add('fade-out');
            setTimeout(() => toast.remove(), 300);
        }, 3000);
    }

    closeBtn.onclick = () => modal.style.display = 'none';
    window.onclick = (event) => { if (event.target == modal) modal.style.display = 'none'; };

    // Shutdown Logic
    const shutdownBtn = document.getElementById('shutdownBtn');
    if (shutdownBtn) {
        shutdownBtn.addEventListener('click', () => {
            if (confirm('Are you sure you want to stop the SectorFlux server?')) {
                API.shutdown()
                    .then(response => {
                        if (response.ok) document.body.innerHTML = '<div style="display:flex;justify-content:center;align-items:center;height:100vh;background:#111;color:#fff;font-family:sans-serif;"><h1>Server Stopped</h1></div>';
                    })
                    .catch(error => console.error('Error shutting down:', error));
            }
        });
    }

    // Cache Toggle Logic
    const cacheToggle = document.getElementById('cacheToggle');
    const cacheStatusText = document.getElementById('cacheStatusText');

    function updateCacheStatusUI(enabled) {
        cacheToggle.checked = enabled;
        cacheStatusText.textContent = enabled ? 'Cache: On' : 'Cache: Off';
        cacheStatusText.style.color = enabled ? '#4ade80' : '#9ca3af';
    }

    if (cacheToggle) {
        // Init
        API.getCacheConfig()
            .then(data => updateCacheStatusUI(data.enabled))
            .catch(err => console.error('Error fetching cache config:', err));

        // Change listener
        cacheToggle.addEventListener('change', async (e) => {
            const enabled = e.target.checked;
            updateCacheStatusUI(enabled); // Optimistic update

            try {
                await API.setCacheConfig(enabled);
            } catch (err) {
                console.error('Error updating cache config:', err);
                updateCacheStatusUI(!enabled);
                alert('Failed to update cache configuration');
            }
        });
    }

    // Tab Switching Logic
    const tabs = document.querySelectorAll('.tab-btn');
    const tabContents = document.querySelectorAll('.tab-content');
    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            tabs.forEach(t => t.classList.remove('active'));
            tabContents.forEach(c => c.classList.remove('active'));
            tab.classList.add('active');
            document.getElementById(`${tab.getAttribute('data-tab')}-view`).classList.add('active');
        });
    });

    // Chat Logic
    const chatInput = document.getElementById('chat-input');
    const sendBtn = document.getElementById('send-btn');
    const chatHistory = document.getElementById('chat-history');
    const modelSelect = document.getElementById('model-select');
    let chatSocket;
    let currentAssistantMessageDiv = null;

    function connectChatWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        chatSocket = new WebSocket(`${protocol}//${window.location.host}/ws/chat`);

        chatSocket.onopen = () => {
            console.log('Connected to Chat WebSocket');
            appendSystemMessage('Connected to SectorFlux Chat Stream.');
        };

        chatSocket.onmessage = (event) => {
            // Simple line-based parsing for NDJSON
            const lines = event.data.split('\n');
            for (const line of lines) {
                if (!line.trim()) continue;
                try {
                    const data = JSON.parse(line);
                    if (data.error) {
                        appendMessage('assistant', `Error: ${data.error}`);
                        return;
                    }
                    if (data.message && data.message.content) {
                        if (!currentAssistantMessageDiv) currentAssistantMessageDiv = appendMessage('assistant', '');
                        currentAssistantMessageDiv.textContent += data.message.content;
                        chatHistory.scrollTop = chatHistory.scrollHeight;
                    }
                    if (data.done) {
                        currentAssistantMessageDiv = null;
                        // No need to fetchLogs here, dashboard socket will handle it
                    }
                } catch (e) { console.error('Error parsing JSON chunk:', e); }
            }
        };

        chatSocket.onclose = () => {
            console.log('Disconnected from Chat WebSocket');
            setTimeout(connectChatWebSocket, 3000);
        };
    }

    function appendMessage(role, text) {
        const msgDiv = document.createElement('div');
        msgDiv.classList.add('message', role === 'user' ? 'user-message' : 'assistant-message');
        msgDiv.textContent = text;
        chatHistory.appendChild(msgDiv);
        chatHistory.scrollTop = chatHistory.scrollHeight;
        return msgDiv;
    }

    function appendSystemMessage(text) {
        const msgDiv = document.createElement('div');
        msgDiv.classList.add('message', 'system-message');
        msgDiv.textContent = text;
        chatHistory.appendChild(msgDiv);
        chatHistory.scrollTop = chatHistory.scrollHeight;
    }

    function sendMessage() {
        const text = chatInput.value.trim();
        if (!text) return;
        if (!chatSocket || chatSocket.readyState !== WebSocket.OPEN) {
            alert('Not connected to chat server.');
            return;
        }
        appendMessage('user', text);
        chatInput.value = '';
        const payload = { model: modelSelect.value || 'llama3', messages: [{ role: 'user', content: text }] };
        chatSocket.send(JSON.stringify(payload));
    }

    if (sendBtn && chatInput) {
        sendBtn.addEventListener('click', sendMessage);
        chatInput.addEventListener('keypress', (e) => {
            if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                sendMessage();
            }
        });
    }

    // Model Management
    const refreshModelsBtn = document.getElementById('refresh-models-btn');
    const runningModelName = document.getElementById('running-model-name');

    async function fetchModels() {
        try {
            const data = await API.fetchModels();
            modelSelect.innerHTML = '';
            if (data.models && data.models.length > 0) {
                data.models.forEach(model => {
                    const option = document.createElement('option');
                    option.value = model.name;
                    option.textContent = model.name;
                    modelSelect.appendChild(option);
                });
            } else {
                const option = document.createElement('option');
                option.textContent = "No models found";
                option.disabled = true;
                modelSelect.appendChild(option);
            }
        } catch (error) {
            console.error('Error fetching models:', error);
            modelSelect.innerHTML = '<option disabled>Error fetching models</option>';
        }
    }

    if (refreshModelsBtn) refreshModelsBtn.addEventListener('click', fetchModels);

    // Dashboard WebSocket
    function connectDashboardWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const ws = new WebSocket(`${protocol}//${window.location.host}/ws/dashboard`);

        ws.onopen = () => console.log('Connected to Dashboard WebSocket');

        ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                if (data.logs) renderLogs(data.logs);
                if (data.metrics) {
                    metricTotal.textContent = data.metrics.total_requests;
                    metricLatency.textContent = `${data.metrics.avg_latency_ms.toFixed(0)}ms`;
                    metricCache.textContent = `${(data.metrics.cache_hit_rate * 100).toFixed(1)}%`;
                }
                if (data.running_model) {
                    if (runningModelName) {
                        runningModelName.textContent = `Running: ${data.running_model}`;
                        runningModelName.className = data.running_model !== 'None' && data.running_model !== 'Ollama Offline'
                            ? 'running-model active' : 'running-model inactive';
                    }
                }
            } catch (e) { console.error('Error parsing dashboard update:', e); }
        };

        ws.onclose = () => {
            console.log('Dashboard WebSocket disconnected. Reconnecting in 2s...');
            setTimeout(connectDashboardWebSocket, 2000);
        };
    }

    // Fetch and display version in footer
    async function fetchVersion() {
        try {
            const data = await API.fetchVersion();
            const versionEl = document.getElementById('footer-version');
            if (versionEl) {
                versionEl.textContent = `SectorFlux v${data.version}`;
            }
        } catch (error) {
            console.error('Error fetching version:', error);
        }
    }

    // Initialize
    fetchLogs();
    fetchMetrics();
    fetchModels();
    fetchVersion();
    connectChatWebSocket();
    connectDashboardWebSocket();
    setInterval(fetchModels, 10000);
});
