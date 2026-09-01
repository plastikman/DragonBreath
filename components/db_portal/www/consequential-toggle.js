(function (global) {
  'use strict';

  function asBool(value) { return value === true; }
  function text(node, value) { node.textContent = value == null ? '' : String(value); }

  function createElement(document, name, className) {
    var node = document.createElement(name);
    if (className) node.className = className;
    return node;
  }

  function mount(root, options) {
    if (!root || !options || typeof options.read !== 'function' || typeof options.mutate !== 'function') {
      throw new TypeError('root, read, and mutate are required');
    }

    var document = root.ownerDocument;
    var authoritative = { value: false, disabled: true, disabledReason: 'Loading authoritative state…' };
    var pending = false;
    var modalOpen = false;
    var generation = 0;

    root.replaceChildren();
    root.classList.add('consequential-toggle');

    var row = createElement(document, 'div', 'ct-row');
    var copy = createElement(document, 'div', 'ct-copy');
    var label = createElement(document, 'label', 'ct-label');
    var description = createElement(document, 'p', 'ct-description');
    var input = createElement(document, 'input', 'ct-input');
    input.type = 'checkbox';
    input.setAttribute('role', 'switch');
    input.id = options.id || ('ct-' + Math.random().toString(36).slice(2));
    label.htmlFor = input.id;
    text(label, options.label);
    text(description, options.description);
    copy.appendChild(label);
    if (options.description) copy.appendChild(description);
    row.appendChild(copy);
    row.appendChild(input);

    var reason = createElement(document, 'p', 'ct-disabled-reason');
    var status = createElement(document, 'p', 'ct-status');
    status.setAttribute('role', 'status');
    status.setAttribute('aria-live', 'polite');
    root.appendChild(row);
    root.appendChild(reason);
    root.appendChild(status);

    function render(state, message, error) {
      authoritative = {
        value: asBool(state && state.value),
        disabled: !!(state && state.disabled),
        disabledReason: state && state.disabledReason ? String(state.disabledReason) : ''
      };
      input.checked = authoritative.value;
      input.disabled = pending || modalOpen || authoritative.disabled;
      input.setAttribute('aria-busy', pending ? 'true' : 'false');
      reason.hidden = !authoritative.disabledReason;
      text(reason, authoritative.disabledReason);
      status.className = 'ct-status' + (error ? ' is-error' : '');
      text(status, message || '');
      if (typeof options.onState === 'function') options.onState(authoritative);
    }

    function showConfirm(spec) {
      return new Promise(function (resolve) {
        var dialog = createElement(document, 'dialog', 'ct-dialog');
        dialog.setAttribute('aria-labelledby', input.id + '-dialog-title');
        dialog.setAttribute('aria-describedby', input.id + '-dialog-body');
        var panel = createElement(document, 'div', 'ct-dialog-panel');
        if (spec.severity) panel.dataset.severity = spec.severity;
        var title = createElement(document, 'h2', 'ct-dialog-title');
        title.id = input.id + '-dialog-title';
        text(title, spec.title || 'Confirm change');
        var body = createElement(document, 'p', 'ct-dialog-body');
        body.id = input.id + '-dialog-body';
        text(body, spec.body || '');
        var actions = createElement(document, 'div', 'ct-dialog-actions');
        var cancel = createElement(document, 'button', 'button ct-cancel');
        cancel.type = 'button';
        text(cancel, spec.cancelLabel || 'Cancel');
        var confirm = createElement(document, 'button', 'button primary ct-confirm');
        confirm.type = 'button';
        text(confirm, spec.confirmLabel || 'Confirm');
        actions.appendChild(cancel);
        actions.appendChild(confirm);
        panel.appendChild(title);
        panel.appendChild(body);
        panel.appendChild(actions);
        dialog.appendChild(panel);
        document.body.appendChild(dialog);

        var settled = false;
        function finish(accepted) {
          if (settled) return;
          settled = true;
          dialog.close();
          dialog.remove();
          resolve(accepted);
          // Native dialog focus restoration runs as the close completes. Move
          // focus back to the initiating switch after that browser work.
          setTimeout(function () { input.focus(); }, 0);
        }
        cancel.addEventListener('click', function () { finish(false); });
        confirm.addEventListener('click', function () { finish(true); });
        dialog.addEventListener('cancel', function (event) {
          event.preventDefault();
          finish(false);
        });
        dialog.addEventListener('keydown', function (event) {
          if (event.key === 'Escape') {
            event.preventDefault();
            finish(false);
            return;
          }
          if (event.key === 'Enter' && document.activeElement !== confirm && document.activeElement !== cancel) {
            event.preventDefault();
          }
        });
        dialog.showModal();
        cancel.focus();
      });
    }

    function reconcile(message, error, token) {
      return Promise.resolve(options.read()).then(function (serverState) {
        if (token !== generation) return authoritative;
        render(serverState, message, error);
        return authoritative;
      }, function (readError) {
        if (token !== generation) return authoritative;
        render(authoritative, message || ((readError && readError.message) || 'Could not refresh state.'), true);
        return authoritative;
      });
    }

    function applyRequested(requested) {
      if (pending || modalOpen)
        return Promise.resolve(authoritative);
      if (authoritative.disabled) {
        render(authoritative, authoritative.disabledReason || '', false);
        return Promise.resolve(authoritative);
      }

      input.checked = authoritative.value;
      var confirmSpec = typeof options.confirm === 'function'
        ? options.confirm(requested, authoritative)
        : null;
      var confirmation = Promise.resolve(true);
      if (confirmSpec) {
        modalOpen = true;
        render(authoritative, '', false);
        confirmation = showConfirm(confirmSpec).then(function (accepted) {
          modalOpen = false;
          render(authoritative, '', false);
          return accepted;
        });
      }

      return confirmation.then(function (accepted) {
        if (!accepted) return authoritative;
        pending = true;
        var token = ++generation;
        render(authoritative, options.pendingMessage || 'Saving…', false);
        return Promise.resolve(options.mutate(requested)).then(function (serverState) {
          if (token !== generation) return authoritative;
          pending = false;
          render(serverState, options.successMessage || 'Saved.', false);
          return authoritative;
        }, function (mutationError) {
          if (token !== generation) return authoritative;
          pending = false;
          var message = (mutationError && mutationError.message) || options.failureMessage || 'Change failed.';
          return reconcile(message, true, token);
        });
      });
    }

    input.addEventListener('change', function () { applyRequested(input.checked); });

    var api = {
      refresh: function () {
        var token = ++generation;
        pending = true;
        render(authoritative, 'Loading…', false);
        return Promise.resolve(options.read()).then(function (serverState) {
          if (token === generation) {
            pending = false;
            render(serverState, '', false);
          }
          return authoritative;
        }, function (error) {
          if (token === generation) {
            pending = false;
            render({ value: authoritative.value, disabled: true, disabledReason: 'Authoritative state is unavailable.' },
              (error && error.message) || 'Could not load state.', true);
          }
          return authoritative;
        });
      },
      request: applyRequested,
      state: function () { return authoritative; },
      elements: { input: input, status: status, reason: reason }
    };
    api.refresh();
    return api;
  }

  global.DBConsequentialToggle = { mount: mount };
}(typeof window !== 'undefined' ? window : globalThis));
