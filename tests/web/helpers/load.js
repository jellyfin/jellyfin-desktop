import { readFileSync } from "node:fs";
import vm from "node:vm";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const webDir = resolve(here, "..", "..", "..", "src", "web");

export function loadScript(relPath, sandbox) {
  const src = readFileSync(resolve(webDir, relPath), "utf8");
  if (!vm.isContext(sandbox)) vm.createContext(sandbox);
  vm.runInContext(src, sandbox, { filename: relPath });
  return sandbox;
}
