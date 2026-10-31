"""Source wiring checks only; these do not load a provider or prove its ABI."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler"


def function(source, marker):
    start = source.index("{", source.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class NativeRouteSource(unittest.TestCase):
    def test_context_calls_keep_the_recorded_owner(self):
        source = (PRODUCT / "inputs/FfxApi_Dx12.cpp").read_text(encoding="utf-8-sig")
        resolver = function(source, "static FfxModule* GameDenoiserFor(")
        self.assertLess(resolver.index("_gameDenoiserContexts.Find"),
                        resolver.index("GameDenoiserDx12"))
        for operation in ("Configure", "Query", "Dispatch"):
            body = function(source, f"ffxReturnCode_t ffx{operation}_Dx12(")
            self.assertIn(f"return game->{operation}(context, desc)", body)
            self.assertIn("if (nativeUnavailable)", body)
            self.assertIn("return FFX_API_RETURN_NO_PROVIDER", body)
        create = function(source, "ffxReturnCode_t ffxCreateContext_Dx12(")
        self.assertIn("result == FFX_API_RETURN_OK && context && *context", create)
        self.assertIn("_gameDenoiserContexts.Insert(*context, game)", create)
        destroy = function(source, "ffxReturnCode_t ffxDestroyContext_Dx12(")
        self.assertIn("const auto original = *context", destroy)
        self.assertIn("_gameDenoiserContexts.Destroy(original", destroy)

    def test_late_loader_is_not_hidden_by_eager_initialization(self):
        source = (PRODUCT / "proxies/FfxApi_Proxy.h").read_text(encoding="utf-8-sig")
        init = function(source, "static bool InitFfxDx12(")
        self.assertLess(init.index("gameLoaderDx12_.store"), init.index("return true"))
        resolve = function(source, "static FfxModule* GameDenoiserDx12(")
        self.assertNotIn("static FfxModule* game", resolve)
        self.assertIn("gameLoaderDirectory_", resolve)
        self.assertIn("if (!exists)", resolve)
        self.assertIn("unavailable = true", resolve)
        self.assertIn("module->dll == denoiser_dx12.dll", resolve)
        self.assertIn("modules.push_back(std::move(module))", resolve)
        hooks = (PRODUCT / "hooks/LibraryLoad_Hooks.cpp").read_text(encoding="utf-8-sig")
        branch = function(hooks, "if (CheckDllNameW(&libName, &ffxDx12NamesW))")
        self.assertLess(branch.index("ObserveGameFfxLoaderPath"), branch.index("return module"))


if __name__ == "__main__":
    unittest.main()
