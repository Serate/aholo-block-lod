interface DocsCodeToolsConfig {
    copyLabel: string;
    copiedLabel: string;
    failedLabel: string;
}

const copyTargetsSelector = '.markdown-body pre, .typedoc-html pre';
const resetTimers = new WeakMap<HTMLButtonElement, number>();

export function mountDocsCodeTools(config: DocsCodeToolsConfig) {
    const targets = Array.from(document.querySelectorAll<HTMLElement>(copyTargetsSelector));

    for (const target of targets) {
        if (target.closest('[data-code-block-copy]') || !getCodeText(target).trim()) {
            continue;
        }

        const shell = document.createElement('div');
        shell.className = 'code-block-shell';
        shell.dataset.codeBlockCopy = '';

        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'code-copy-button';
        button.dataset.copyState = 'idle';
        setButtonState(button, config.copyLabel, 'idle');
        button.addEventListener('click', () => {
            void copyCode(target, button, config);
        });

        target.before(shell);
        shell.append(target, button);
    }
}

async function copyCode(target: HTMLElement, button: HTMLButtonElement, config: DocsCodeToolsConfig) {
    const text = getCodeText(target);

    try {
        await writeClipboard(text);
        setButtonState(button, config.copiedLabel, 'copied');
        scheduleReset(button, config.copyLabel);
    } catch {
        setButtonState(button, config.failedLabel, 'failed');
        scheduleReset(button, config.copyLabel);
    }
}

function getCodeText(target: HTMLElement) {
    return target.querySelector('code')?.textContent ?? target.textContent ?? '';
}

async function writeClipboard(text: string) {
    if (navigator.clipboard?.writeText) {
        try {
            await navigator.clipboard.writeText(text);
            return;
        } catch {
            // Fall back for browsers that expose Clipboard API but deny this call.
        }
    }

    const textarea = document.createElement('textarea');
    const activeElement = document.activeElement instanceof HTMLElement ? document.activeElement : null;

    textarea.value = text;
    textarea.setAttribute('readonly', '');
    textarea.style.position = 'fixed';
    textarea.style.top = '-999px';
    textarea.style.left = '-999px';
    textarea.style.width = '1px';
    textarea.style.height = '1px';
    textarea.style.opacity = '0';
    document.body.append(textarea);
    textarea.focus({ preventScroll: true });
    textarea.select();
    textarea.setSelectionRange(0, text.length);

    const clipboardFallback = document as unknown as { execCommand(commandId: string): boolean };
    const copied = clipboardFallback.execCommand('copy');
    textarea.remove();
    activeElement?.focus({ preventScroll: true });

    if (!copied) {
        throw new Error('Copy failed');
    }
}

function scheduleReset(button: HTMLButtonElement, copyLabel: string) {
    const existingTimer = resetTimers.get(button);

    if (existingTimer !== undefined) {
        window.clearTimeout(existingTimer);
    }

    resetTimers.set(
        button,
        window.setTimeout(() => {
            setButtonState(button, copyLabel, 'idle');
            resetTimers.delete(button);
        }, 1600),
    );
}

function setButtonState(button: HTMLButtonElement, label: string, state: string) {
    button.textContent = label;
    button.title = label;
    button.setAttribute('aria-label', label);
    button.dataset.copyState = state;
}
