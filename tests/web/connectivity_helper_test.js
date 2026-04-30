import { test } from "node:test";
import { strict as assert } from "node:assert";
import { loadScript } from "./helpers/load.js";
import { makeWindow, makeJmpBus, lastSend, sendsOf } from "./helpers/mocks.js";

function setup() {
  const jmp = makeJmpBus();
  const win = makeWindow({ jmp });
  loadScript("connectivityHelper.js", win);
  return { win, jmp, check: win.jmpCheckServerConnectivity };
}

test("exposes jmpCheckServerConnectivity as a function with .abort", () => {
  const { check } = setup();
  assert.equal(typeof check, "function");
  assert.equal(typeof check.abort, "function");
});

test("success path: bus inbound resolves with baseUrl", async () => {
  const { jmp, check } = setup();
  const promise = check("http://example");
  assert.deepEqual(lastSend(jmp, "overlay.checkServerConnectivity"), { url: "http://example" });

  jmp.deliver("overlay.serverConnectivityResult", {
    url: "http://example",
    success: true,
    baseUrl: "http://example/resolved",
  });
  const result = await promise;
  assert.equal(result, "http://example/resolved");
});

test("failure path: bus inbound rejects", async () => {
  const { jmp, check } = setup();
  const promise = check("http://x");
  jmp.deliver("overlay.serverConnectivityResult", { url: "http://x", success: false, baseUrl: "" });
  await assert.rejects(promise, /Connection failed/);
});

test("stale-URL callback (mismatched URL) is ignored", async () => {
  const { jmp, check } = setup();
  const promise = check("http://current");
  jmp.deliver("overlay.serverConnectivityResult", {
    url: "http://previous",
    success: true,
    baseUrl: "ignored",
  });

  const sentinel = new Promise((r) => setTimeout(() => r("timeout"), 30));
  const winner = await Promise.race([promise, sentinel]);
  assert.equal(winner, "timeout");

  jmp.deliver("overlay.serverConnectivityResult", {
    url: "http://current",
    success: true,
    baseUrl: "resolved",
  });
  assert.equal(await promise, "resolved");
});

test("abort() emits overlay.cancelServerConnectivity and rejects the pending promise", async () => {
  const { jmp, check } = setup();
  const promise = check("http://x");
  check.abort();
  assert.equal(sendsOf(jmp, "overlay.cancelServerConnectivity").length, 1);
  await assert.rejects(promise, /Connection cancelled/);
});
