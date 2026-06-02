import child_process from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const packages = JSON.parse(
    child_process.execSync('pnpm list --filter=@manycore/* -r -depth -1 --json', { stdio: 'pipe' }).toString('utf-8'),
).filter(item => !item.private);

for (const p of packages) {
    const cwd = p.path;
    const packageJson = JSON.parse(fs.readFileSync(path.resolve(cwd, 'package.json'), 'utf-8'));
    const hiddenBuildCommand = packageJson.scripts?.['.build'];
    let published = false;
    try {
        child_process.execSync(`npm view ${p.name}@${p.version}`, { stdio: 'ignore' });
        published = true;
    } catch {
        // assume not found. should publish.
    }
    if (!published) {
        if (hiddenBuildCommand) {
            // hidden build command exists, add build commands to call .build
            packageJson.scripts.build = 'pnpm run .build';
            fs.writeFileSync(path.resolve(cwd, 'package.json'), JSON.stringify(packageJson, undefined, 2), 'utf-8');
        }
        // run build command if exists
        child_process.execSync('pnpm run --if-present build', { stdio: 'inherit', cwd });
        // cleanup package.json before publish
        child_process.execSync('npm pkg delete scripts devDependencies', { stdio: 'inherit', cwd });
        child_process.execSync('npm publish --access public', { stdio: 'inherit', cwd });
    }
}
