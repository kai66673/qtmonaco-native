# pydemo — Python-аналог qmn-demo на колесе

Tab-редактор на том же виджете, что и C++ `qmn-demo`, но собранный не
CMake, а `uv`: зависимость — только что выпущенный wheel
`qmn_native` (см. корневой README, «Python-пакет»).

```
pydemo/
├── pyproject.toml   # uv-проект; qmn-native — path-зависимость на ../dist/repaired/*.whl
└── main.py          # аналог src/app/{MainWindow,EditorSettings*,demo_main}.cpp
```

## Как запустить

1. Собрать wheel (контейнерный пайплайн, требуется Docker):

   ```bash
   ./bindings/build_in_container.sh --wheel
   ```

2. В этой директории — `uv` поднимет окружение сам:

   ```bash
   cd pydemo
   uv run main.py [files...]
   ```

Headless-прогрев, как у C++-демо (тот же флаг `--exit-after-ms`, тот же
`QT_QPA_PLATFORM=offscreen`):

```bash
QT_QPA_PLATFORM=offscreen uv run main.py ../examples/demo.py ../examples/demo.json \
    ../examples/demo.js ../examples/demo.c ../examples/demo.cpp --exit-after-ms 400
```

## Что это такое

Функциональный аналог C++ `qmn-demo` дверь-в-дверь: вкладки редакторов,
меню «Файл» (Открыть…, Закрыть вкладку) и «Настройки» (Параметры
редактора…), статус «Стр. N, кол. M · выделено K», шаблон настроек
(диалог сейдится с текущего редактора, ОК разливает шаблон по всем
вкладкам и будущим вкладкам). Из 15 knobs настроек 14 идут через
`options()` (двери M19/M20/M21/M25), 15-й — пресет темы M24 — через
`editor.theme()`, ровно как в C++-демо.

## Осознанные отличия от C++-демо

- **Требование к колесу >= 1.6.1:** 15-й knob (тема, M24) едет на
  `editor.theme()` — `CodeEditorTheme` перешёл границу wheel-привязок
  только в 1.6.1. На более старом колесе pydemo не запустится
  (`AttributeError` на `theme()`/`CodeEditorTheme` — честный, не no-op).
  14-й options-knob (перенос строк, M25) требует колеса, собранного уже
  после M25 (с 1.7.0) — там та же история: `AttributeError` на
  `setSoftWrapEnabled`, тоже честный.
- `uv.lock` намеренно не коммитится: wheel лежит в gitignore-нутом
  `dist/` (это артефакт релиза, не исходник).
- Путь к колесу в `pyproject.toml` bump-ится вручную при каждом релизе
  (uv не принимает glob в path-sources; в CI pydemo не гоняется).
- Тема/локаль как в C++-демо: русский интерфейс, UTF-8 без BOM,
  CRLF→LF на загрузке; кодировки с автоопределением — общий для обоих
  демо будущий milestone.
