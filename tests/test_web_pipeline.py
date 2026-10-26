import unittest
import os
import subprocess

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

class TestWebPipeline(unittest.TestCase):
    def test_web_index_exists(self):
        index_path = os.path.join(BASE_DIR, "web", "index.html")
        self.assertTrue(os.path.exists(index_path))
        with open(index_path, "r") as f:
            content = f.read()
        self.assertIn('<canvas id="game-canvas"', content)
        self.assertIn('id="api-badge"', content)

    def test_js_modules_exist(self):
        modules = [
            "webgpu_renderer.js",
            "webaudio_sink.js",
            "vfs.js",
            "engine_bridge.js",
            "wasm_engine_worker.js",
            "app.js"
        ]
        for mod in modules:
            p = os.path.join(BASE_DIR, "web", mod)
            self.assertTrue(os.path.exists(p), f"Module {mod} missing")
            res = subprocess.run(["node", "--check", p], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Syntax error in {mod}: {res.stderr}")

    def test_engine_binaries_deployed(self):
        so_path = os.path.join(BASE_DIR, "web", "engine", "libpayback2.so")
        uc_path = os.path.join(BASE_DIR, "web", "engine", "unicorn_aarch64.js")
        wasm_path = os.path.join(BASE_DIR, "web", "engine", "pb_shims.wasm")
        js_path = os.path.join(BASE_DIR, "web", "engine", "pb_shims.js")

        self.assertTrue(os.path.exists(so_path), "libpayback2.so missing from web/engine")
        self.assertTrue(os.path.exists(uc_path), "unicorn_aarch64.js missing from web/engine")
        self.assertTrue(os.path.exists(wasm_path), "pb_shims.wasm missing from web/engine")
        self.assertTrue(os.path.exists(js_path), "pb_shims.js missing from web/engine")

        self.assertGreater(os.path.getsize(so_path), 30_000_000)
        self.assertGreater(os.path.getsize(uc_path), 1_000_000)
        self.assertGreater(os.path.getsize(wasm_path), 10_000)

    def test_assets_symlink(self):
        amct_path = os.path.join(BASE_DIR, "web", "assets", "Data", "Additional", "Glyphs", "FE_Small_High.amct")
        self.assertTrue(os.path.exists(amct_path), f"Asset {amct_path} not accessible via symlink")
        self.assertGreater(os.path.getsize(amct_path), 10000)

    def test_oracle_dumps(self):
        save_path = os.path.join(BASE_DIR, "oracle_dumps", "Backup.sav")
        ini_path = os.path.join(BASE_DIR, "oracle_dumps", "Payback.ini")
        self.assertTrue(os.path.exists(save_path))
        self.assertTrue(os.path.exists(ini_path))
        self.assertEqual(os.path.getsize(save_path), 26744)

    def test_unicorn_wasm_loader_execution(self):
        test_script = os.path.join(BASE_DIR, "web", "test_unicorn_loader.js")
        res = subprocess.run(["node", test_script], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Unicorn loader test failed: {res.stderr}")
        self.assertIn("Applied 3891 R_AARCH64_RELATIVE relocations", res.stdout)
        self.assertIn("libpayback2.so is mapped and relocated in WebAssembly", res.stdout)

    def test_pb_shims_wasm_init(self):
        cmd = "import('./engine/pb_shims.js').then(async m => { const mod = await m.default(); const res = mod._pb_wasm_init(); if (res !== 0) process.exit(1); });"
        res = subprocess.run(["node", "-e", cmd], cwd=os.path.join(BASE_DIR, "web"), capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"pb_shims.wasm init failed: {res.stderr}")
        self.assertIn("Shims successfully initialized!", res.stdout)

if __name__ == "__main__":
    unittest.main()
