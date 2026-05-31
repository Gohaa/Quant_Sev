/**
 * Quant_Sev HTTP 桥 — Phase 1 骨架
 * 提供 Transport、Logger、Account 页基础绑定
 */
(function (global) {
    'use strict';

    var cachedHttpPort = null;

    function resolveApiUrl(url) {
        if (/^https?:\/\//i.test(url)) {
            return url;
        }
        var base = '';
        try {
            if (global.location && global.location.protocol && global.location.protocol.indexOf('http') === 0) {
                base = global.location.origin;
            }
        } catch (e) { /* ignore */ }
        if (!base) {
            try {
                base = global.localStorage && localStorage.getItem('quant_sev_api_base');
            } catch (e2) { /* ignore */ }
        }
        if (!base) {
            var port = cachedHttpPort || 8080;
            base = 'http://127.0.0.1:' + port;
        }
        return base + (url.charAt(0) === '/' ? url : '/' + url);
    }

    var Transport = {
        isHttp: function () { return true; },
        setHttpPort: function (port) {
            if (port) cachedHttpPort = port;
        },
        buildUrl: function (url, params) {
            if (!params || typeof params !== 'object') return url;
            var parts = [];
            Object.keys(params).forEach(function (key) {
                var v = params[key];
                if (v === undefined || v === null || v === '') return;
                parts.push(encodeURIComponent(key) + '=' + encodeURIComponent(String(v)));
            });
            if (!parts.length) return url;
            return url + (url.indexOf('?') >= 0 ? '&' : '?') + parts.join('&');
        },
        get: function (url, params) {
            var fullUrl = resolveApiUrl(Transport.buildUrl(url, params));
            return fetch(fullUrl, { method: 'GET', headers: { 'Accept': 'application/json' } })
                .then(function (res) {
                    return res.json().catch(function () { return {}; }).then(function (data) {
                        if (!res.ok) {
                            var msg = (data && (data.error || data.message)) || ('HTTP ' + res.status);
                            if (data && data.path) msg += ' (' + data.path + ')';
                            throw new Error(msg);
                        }
                        if (url.indexOf('/api/status') >= 0 && data && data.http_port) {
                            Transport.setHttpPort(data.http_port);
                        }
                        return data;
                    });
                });
        },
        post: function (url, body) {
            return fetch(resolveApiUrl(url), {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'Accept': 'application/json' },
                body: JSON.stringify(body || {})
            }).then(function (res) {
                return res.json().catch(function () { return {}; }).then(function (data) {
                    if (!res.ok) {
                        var msg = (data && (data.error || data.message)) || ('HTTP ' + res.status);
                        if (data && data.path) msg += ' (' + data.path + ')';
                        throw new Error(msg);
                    }
                    return data;
                });
            });
        }
    };

    function postLog(line) {
        try {
            var frames = document.querySelectorAll('iframe.logger-embed-frame');
            frames.forEach(function (f) {
                if (f.contentWindow) {
                    f.contentWindow.postMessage({ source: 'QuantSevLogger', type: 'append', line: line }, '*');
                }
            });
        } catch (e) { /* ignore */ }
    }

    var Logger = {
        Transport: Transport,
        log: function (msg) { postLog('[INFO] ' + msg); },
        warn: function (msg) { postLog('[WARN] ' + msg); },
        error: function (msg) { postLog('[ERROR] ' + msg); },
        autoInitPage: function () {
            if (document.getElementById('account-list')) {
                AccountPage.init();
            }
        }
    };

    var AccountPage = {
        selectedUserId: null,
        selectedName: null,
        accountsCache: [],
        init: function () {
            var self = this;
            self.bindButtons();
            self.refreshList();
            setInterval(function () { self.refreshList(); }, 4000);
        },
        bindButtons: function () {
            var self = this;
            var saveBtn = document.getElementById('btn-save-acc');
            if (saveBtn) {
                saveBtn.addEventListener('click', function () { self.saveAccount(); });
            }
            var mdOn = document.getElementById('btn-md-connect');
            var mdOff = document.getElementById('btn-md-disconnect');
            var tdOn = document.getElementById('btn-td-connect');
            var tdOff = document.getElementById('btn-td-disconnect');
            if (mdOn) mdOn.addEventListener('click', function () { self.connect('md'); });
            if (mdOff) mdOff.addEventListener('click', function () { self.disconnect('md'); });
            if (tdOn) tdOn.addEventListener('click', function () { self.connect('td'); });
            if (tdOff) tdOff.addEventListener('click', function () { self.disconnect('td'); });
        },
        statusCell: function (connected) {
            if (connected) {
                return '<span class="status-connected" title="已连接">🟢</span>';
            }
            return '<span class="status-disconnected" title="未连接">🔴</span>';
        },
        listHas: function (list, value) {
            if (!list || !value) return false;
            for (var i = 0; i < list.length; i++) {
                if (String(list[i]) === String(value)) return true;
            }
            return false;
        },
        resolveMdConnected: function (acc, st) {
            if (!acc) return false;
            if (acc.md_connected === true) return true;
            if (!st || !st.md_ok) return false;
            if (this.listHas(st.connected_md_fronts, acc.md_front)) return true;
            if (this.listHas(st.connected_md_users, acc.user_id)) return true;
            return false;
        },
        resolveTdConnected: function (acc, st) {
            if (!acc) return false;
            if (acc.td_connected === true) return true;
            if (!st || !st.td_ok) return false;
            return this.listHas(st.connected_td_users, acc.user_id);
        },
        connectPayload: function () {
            return {
                name: val('acc-name'),
                user_id: val('acc-user'),
                md_front: val('acc-md-front'),
                td_front: val('acc-td-front'),
                trader_front: val('acc-td-front')
            };
        },
        refreshList: function () {
            var self = this;
            var tbody = document.getElementById('account-list');
            if (!tbody) return;
            var keepName = self.selectedName;
            Promise.all([
                Transport.get('/api/saved_accounts'),
                Transport.get('/api/status')
            ]).then(function (results) {
                var data = results[0] || {};
                var st = results[1] || {};
                var accounts = data.accounts || [];
                self.accountsCache = accounts;
                self.lastStatus = st;
                tbody.innerHTML = accounts.map(function (acc) {
                    var accName = acc.name || '';
                    var mdConn = self.resolveMdConnected(acc, st);
                    var tdConn = self.resolveTdConnected(acc, st);
                    return '<tr data-user="' + escapeHtml(acc.user_id || '') + '" data-name="' + escapeHtml(accName) + '">' +
                        '<td>' + escapeHtml(accName) + '</td>' +
                        '<td>' + escapeHtml(acc.user_id || '') + '</td>' +
                        '<td>' + escapeHtml(acc.account_type || 'sim') + '</td>' +
                        '<td class="md-status">' + self.statusCell(mdConn) + '</td>' +
                        '<td class="td-status">' + self.statusCell(tdConn) + '</td>' +
                        '</tr>';
                }).join('');
                tbody.querySelectorAll('tr[data-name]').forEach(function (tr) {
                    tr.addEventListener('click', function () {
                        self.selectRow(tr.getAttribute('data-name'));
                    });
                });
                if (keepName) {
                    self.highlightRow(keepName);
                }
            }).catch(function (e) {
                tbody.innerHTML = '<tr><td colspan="5">加载失败: ' + escapeHtml(e.message) + '</td></tr>';
                Logger.error('加载账户列表失败: ' + e.message);
            });
        },
        highlightRow: function (name) {
            var tbody = document.getElementById('account-list');
            if (!tbody || !name) return;
            tbody.querySelectorAll('tr[data-name]').forEach(function (tr) {
                tr.classList.toggle('selected', tr.getAttribute('data-name') === name);
            });
        },
        updateRowConnectionStatus: function (name, kind, connected) {
            var self = this;
            var tbody = document.getElementById('account-list');
            if (!tbody || !name) return;
            tbody.querySelectorAll('tr[data-name]').forEach(function (tr) {
                if (tr.getAttribute('data-name') !== name) return;
                var cell = tr.querySelector(kind === 'md' ? '.md-status' : '.td-status');
                if (cell) cell.innerHTML = self.statusCell(connected);
            });
        },
        selectRow: function (name) {
            var acc = this.accountsCache.find(function (a) { return a.name === name; });
            if (!acc) return;
            this.selectedName = name;
            this.selectedUserId = acc.user_id;
            this.highlightRow(name);
            try {
                if (global.localStorage) localStorage.setItem('quant_sev_user_id', acc.user_id);
            } catch (e) { /* ignore */ }
            setVal('acc-name', acc.name);
            setVal('acc-user', acc.user_id);
            setVal('acc-md-front', acc.md_front);
            setVal('acc-td-front', acc.td_front || acc.trader_front);
            setVal('acc-broker', acc.broker_id);
            setVal('acc-appid', acc.app_id);
            setVal('acc-auth', acc.auth_code);
            setVal('acc-ctp-pass', '');
        },
        saveAccount: function () {
            var payload = {
                name: val('acc-name'),
                user_id: val('acc-user') || val('acc-name'),
                broker_id: val('acc-broker'),
                md_front: val('acc-md-front'),
                td_front: val('acc-td-front'),
                trader_front: val('acc-td-front'),
                app_id: val('acc-appid'),
                auth_code: val('acc-auth'),
                account_type: 'sim'
            };
            var pass = val('acc-ctp-pass');
            if (pass) payload.password = pass;
            Transport.post('/api/save_account', payload).then(function () {
                Logger.log('账户已保存');
                AccountPage.refreshList();
            }).catch(function (e) {
                Logger.error('保存失败: ' + e.message);
            });
        },
        connect: function (kind) {
            var self = this;
            var path = kind === 'md' ? '/api/load/md' : '/api/load/td';
            var payload = self.connectPayload();
            if (!payload.user_id) {
                Logger.warn('请先在列表中点击选择账户');
                return;
            }
            var accName = self.selectedName || payload.name;
            Transport.post(path, payload)
                .then(function (res) {
                    if (res && res.ok === false) {
                        throw new Error(res.error || '连接失败');
                    }
                    Logger.log((res && res.message) || (kind.toUpperCase() + ' 连接成功'));
                    if (kind === 'md' && res && res.symbol_subscribe) {
                        Logger.log('Symbol: ' + res.symbol_subscribe);
                    }
                    if (kind === 'md' && res && res.symbol_subscribe_error) {
                        Logger.warn('Symbol 订阅: ' + res.symbol_subscribe_error);
                    }
                    if (accName) {
                        var mdOk = kind === 'md' && (res.md_connected === true || res.ok !== false);
                        var tdOk = kind === 'td' && (res.td_connected === true || res.ok !== false);
                        self.updateRowConnectionStatus(accName, kind, mdOk || tdOk);
                    }
                    self.refreshList();
                    try {
                        if (global.parent && global.parent !== global && global.parent.pollMainStatus) {
                            global.parent.pollMainStatus();
                        }
                    } catch (e) { /* ignore */ }
                })
                .catch(function (e) { Logger.warn(kind.toUpperCase() + ' 连接: ' + e.message); });
        },
        disconnect: function (kind) {
            var self = this;
            var path = kind === 'md' ? '/api/disconnect/md' : '/api/disconnect/td';
            var payload = self.connectPayload();
            if (!payload.user_id) {
                Logger.warn('请先在列表中点击选择账户');
                return;
            }
            var accName = self.selectedName || payload.name;
            Transport.post(path, payload)
                .then(function (res) {
                    if (res && res.ok === false) {
                        throw new Error(res.error || '断开失败');
                    }
                    Logger.log((res && res.message) || (kind.toUpperCase() + ' 已断开'));
                    if (accName) {
                        self.updateRowConnectionStatus(accName, kind, false);
                    }
                    self.refreshList();
                })
                .catch(function (e) { Logger.warn(kind.toUpperCase() + ' 断开: ' + e.message); });
        }
    };

    function val(id) {
        var el = document.getElementById(id);
        return el ? el.value.trim() : '';
    }
    function setVal(id, v) {
        var el = document.getElementById(id);
        if (el) el.value = v || '';
    }
    function escapeHtml(s) {
        return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Logger = Logger;
    global.QuantSevBridge.Transport = Transport;
})(typeof window !== 'undefined' ? window : this);
