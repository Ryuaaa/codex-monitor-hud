import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";

const [version, tag, archiveName, signaturePath, outputPath] = process.argv.slice(2);
const safeVersion = /^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/;
const safeAsset = /^[0-9A-Za-z ._+-]+\.app\.tar\.gz$/;
if (!safeVersion.test(version ?? "") || tag !== `task-center-v${version}` || !safeAsset.test(archiveName ?? "")) {
  throw new Error("invalid task center release identity");
}
const signature = readFileSync(resolve(signaturePath), "utf8").trim();
if (!signature || signature.length > 4096 || signature.includes("\0")) {
  throw new Error("invalid updater signature");
}
const encodedTag = encodeURIComponent(tag);
const encodedAsset = encodeURIComponent(archiveName).replaceAll("%20", "%20");
const url = `https://github.com/Ryuaaa/codex-monitor-hud/releases/download/${encodedTag}/${encodedAsset}`;
const manifest = {
  version,
  notes: "新增标签与保存筛选、父子/阻塞/相关关系、评论与人工活动，并加入独立安全更新通道。",
  pub_date: new Date().toISOString(),
  platforms: {
    "darwin-aarch64": { signature, url },
    "darwin-x86_64": { signature, url },
  },
};
const output = resolve(outputPath);
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, `${JSON.stringify(manifest, null, 2)}\n`, { encoding: "utf8", mode: 0o644 });
