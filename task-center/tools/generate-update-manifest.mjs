import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";

const [version, tag, archiveName, signaturePath, assetId, outputPath] = process.argv.slice(2);
const safeVersion = /^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/;
const safeAsset = /^[0-9A-Za-z ._+-]+\.app\.tar\.gz$/;
const safeAssetId = /^[1-9]\d*$/;
if (
  !safeVersion.test(version ?? "") ||
  tag !== `task-center-v${version}` ||
  !safeAsset.test(archiveName ?? "") ||
  !safeAssetId.test(assetId ?? "")
) {
  throw new Error("invalid task center release identity");
}
const signature = readFileSync(resolve(signaturePath), "utf8").trim();
if (!signature || signature.length > 4096 || signature.includes("\0")) {
  throw new Error("invalid updater signature");
}
const url = `https://api.github.com/repos/Ryuaaa/codex-monitor-hud/releases/assets/${assetId}`;
const manifest = {
  version,
  notes: "可从任务中心安全继续官方 Codex 任务，并可准确打开对应 Codex 任务；保留按需运行、显式确认和失败隔离。",
  pub_date: new Date().toISOString(),
  platforms: {
    "darwin-aarch64": { signature, url },
    "darwin-x86_64": { signature, url },
  },
};
const output = resolve(outputPath);
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, `${JSON.stringify(manifest, null, 2)}\n`, { encoding: "utf8", mode: 0o644 });
