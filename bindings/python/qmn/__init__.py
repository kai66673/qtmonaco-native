"""Python-facing package for the native Qt editor widget (qtmonaco-native).

The Shiboken-generated extension module is called ``_qmn``, and because the C++
classes live in the namespace ``qmn::view``, its own import path is
``_qmn.qmn.view.CodeEditor`` (Shiboken 6.12 mirrors C++ namespaces as Python
namespaces). This package flattens that to ``qmn.CodeEditor`` and is the single
place where the qtmonaco-compatible surface will live: qtmonaco is 1-based while
the C++ API is 0-based UTF-16 code units, and that conversion - plus the
``set_vim_mode_enabled``/``set_minimap_enabled`` warnings - is deliberately done
here, in Python, not in C++ (PLAN.md, AGENTS.md).

Attribute access (not ``from _qmn.qmn.view import ...``) is on purpose: the
namespace shims are attributes created by the extension at import time, not real
submodules, so the import-statement form would go through the import machinery
and fail.
"""

# Imported first on purpose: the generated wrapper resolves its QWidget base
# through shiboken's converter registry, so PySide6.QtWidgets must already be
# loaded when _qmn initializes.
import PySide6.QtWidgets  # noqa: F401
import _qmn as _ext

CodeEditor = _ext.qmn.view.CodeEditor
TextPosition = _ext.qmn.view.TextPosition

__all__ = ["CodeEditor", "TextPosition"]
