import { spawn } from "node:child_process";
import { createInterface } from "node:readline";

const DEFAULT_TIMEOUT_MS = 20_000;

export class AppServerClient {
  constructor({
    command,
    args = ["app-server", "--listen", "stdio://"],
    env = {},
    unsetEnv = [],
    spawnImpl = spawn,
  }) {
    this.command = command;
    this.args = args;
    this.env = { ...env };
    this.unsetEnv = [...unsetEnv];
    this.spawnImpl = spawnImpl;
    this.process = null;
    this.pending = new Map();
    this.nextId = 1;
    this.notifications = [];
    this.stderr = [];
  }

  async start() {
    if (this.process) {
      throw new Error("app-server is already running");
    }

    const childEnv = { ...process.env, ...this.env };
    const unsetNames = new Set(this.unsetEnv.map((name) => name.toLowerCase()));
    for (const name of Object.keys(childEnv)) {
      if (unsetNames.has(name.toLowerCase())) delete childEnv[name];
    }
    const child = this.spawnImpl(this.command, this.args, {
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true,
      env: childEnv,
    });
    this.process = child;

    const stdout = createInterface({ input: child.stdout });
    stdout.on("line", (line) => this.#handleLine(line));

    const stderr = createInterface({ input: child.stderr });
    stderr.on("line", (line) => {
      this.stderr.push(line);
      if (this.stderr.length > 50) this.stderr.shift();
    });

    child.once("exit", (code, signal) => {
      const detail = signal ? `signal ${signal}` : `exit code ${code}`;
      const error = new Error(`codex app-server stopped with ${detail}`);
      for (const item of this.pending.values()) {
        clearTimeout(item.timer);
        item.reject(error);
      }
      this.pending.clear();
      this.process = null;
    });

    child.once("error", (error) => {
      for (const item of this.pending.values()) {
        clearTimeout(item.timer);
        item.reject(error);
      }
      this.pending.clear();
    });

    await this.request("initialize", {
      clientInfo: {
        name: "usage_meter_host",
        title: "Usage Meter Host",
        version: "0.1.0",
      },
      capabilities: {},
    });
    this.notify("initialized", {});
  }

  request(method, params, timeoutMs = DEFAULT_TIMEOUT_MS) {
    if (!this.process?.stdin?.writable) {
      return Promise.reject(new Error("app-server is not running"));
    }

    const id = this.nextId++;
    const message = params === undefined ? { method, id } : { method, id, params };
    this.process.stdin.write(`${JSON.stringify(message)}\n`);

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${method} timed out after ${timeoutMs} ms`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
    });
  }

  notify(method, params) {
    if (!this.process?.stdin?.writable) {
      throw new Error("app-server is not running");
    }
    const message = params === undefined ? { method } : { method, params };
    this.process.stdin.write(`${JSON.stringify(message)}\n`);
  }

  async stop() {
    const child = this.process;
    if (!child) return;

    child.stdin.end();
    await new Promise((resolve) => {
      const timer = setTimeout(() => {
        child.kill();
        resolve();
      }, 2_000);
      child.once("exit", () => {
        clearTimeout(timer);
        resolve();
      });
    });
  }

  #handleLine(line) {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      return;
    }

    if (message.id !== undefined) {
      const item = this.pending.get(message.id);
      if (!item) return;
      this.pending.delete(message.id);
      clearTimeout(item.timer);
      if (message.error) {
        const error = new Error(message.error.message ?? "app-server request failed");
        error.code = message.error.code;
        error.data = message.error.data;
        item.reject(error);
      } else {
        item.resolve(message.result);
      }
      return;
    }

    if (message.method) {
      this.notifications.push(message);
      if (this.notifications.length > 100) this.notifications.shift();
    }
  }
}
