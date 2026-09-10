"""Loads custom_components/grohe_dial/{const,api}.py as standalone
modules, without ever executing grohe_dial/__init__.py (which imports
Home Assistant unconditionally at module level, unavailable in this
lightweight aiohttp-only test environment -- see test_api.py's own
header comment). const.py/api.py themselves have zero Home Assistant
imports, so this is a real, non-mocked import of the actual production
code, not a stand-in for it.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    """pytest-homeassistant-custom-component blocks loading anything
    under custom_components/ unless this fixture is explicitly pulled in
    -- the standard, documented pattern for testing a real custom
    integration end-to-end against the actual config_entries loader
    rather than a mock of it.
    """
    yield

_COMPONENT_DIR = Path(__file__).resolve().parent.parent / "custom_components" / "grohe_dial"


def _load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(f"grohe_dial.{name}", _COMPONENT_DIR / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[f"grohe_dial.{name}"] = module
    spec.loader.exec_module(module)
    return module


# Order matters: api.py's own `from .const import API_TOKEN_HEADER` resolves
# against sys.modules["grohe_dial.const"], which must exist first.
sys.modules.setdefault("grohe_dial", type(sys)("grohe_dial"))
const = _load("const", "const.py")
sys.modules["grohe_dial"].const = const
api = _load("api", "api.py")
sys.modules["grohe_dial"].api = api
