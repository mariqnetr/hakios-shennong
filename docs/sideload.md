# .ipa прямо на телефоне: TrollStore на HackiOS (shennong)

## Почему TrollStore и почему он тут почти работает сам

* Ядро подписи не проверяет вообще: `libkern/amfi/miniamfi.c` — заглушка AMFI без trust cache, любая
  подпись принимается, ad-hoc получает `CS_PLATFORM_BINARY`. Entitlements берутся из подписи бинарника
  как есть (`miniamfi_entitlements.cpp`), никто за них не ручается. Sandbox тоже заглушка.
* То есть то, ради чего TrollStore эксплуатирует баг CoreTrust, здесь даётся бесплатно. Его «обход CoreTrust»
  здесь просто ни на что не влияет.
* installd в системе есть (job `com.apple.mobile.installd`), но через него идти незачем: TrollStore ставит
  приложения сам: распаковывает в `/var/containers/Bundle/Application/<UUID>/`, создаёт контейнер данных
  через containermanagerd и регистрирует приложение в LaunchServices.
* `/private/var` теперь лежит на флеше (`var_apfs`), так что поставленное переживает
  перезагрузку.

**Единственное, что ломало TrollStore:** свой root-helper (`trollstorehelper`) он запускает через
`posix_spawnattr_set_persona_np(99, OVERRIDE)` с uid 0. Ядро отказывает в spawn в несуществующую persona
(`kern_exec.c`, `spawn_validate_persona` → `ESRCH`), а персон в системе нет вообще: в логе launchd
`kpersona_find_by_type() failed ... No such process`. Так что ни одна установка не дошла бы до helper'а.

## Что здесь лежит

| Файл | Что |
|---|---|
| `tsboot.c` → `tsboot` | Запускается первым при загрузке (`build/boot-bin`). Создаёт persona 99: `kpersona_alloc` нужен только entitlement `com.apple.private.persona-mgmt`, root не нужен. Дальше работает в фоне, список задач идёт дальше сразу. Через 40 с: если TrollStore на флеше нет, выполняет `trollstorehelper install-trollstore TrollStore.tar`, если есть, то `trollstorehelper refresh` (перерегистрирует TrollStore и всё, что он поставил; не `refresh-all`: тот перестраивает всю базу LaunchServices и делает killall backboardd, из-за чего SpringBoard перезапускался через минуту после загрузки). Всё печатает в лог с префиксом `tsboot:` |
| `tsboot.entitlements` | `persona-mgmt` + `platform-application` |
| `sign.py`, `libSystem.tbd` | как собран `tsboot` без Xcode и ldid: `clang -target arm64-apple-ios16.0` + `ld64.lld` + своя ad-hoc подпись с entitlements |
| `trollstore/TrollStore.tar` | TrollStore 2 (последний релиз opa334/TrollStore), sha256 `144dc741…0551` |
| `trollstore/trollstorehelper` | helper из этого tar: им `tsboot` ставит TrollStore в первый раз |
| `enable.sh` | включает или выключает всё это для следующей сборки |

В `build-ios.sh` добавлены 7 строк (копия до правки: `build-ios.sh.before-trollstore`): если есть
`build/trollstore`, он идёт в образ как `/redwood/share/trollstore` (через `EXTRA_SHARE` в
`userland/build.sh`). Без этой папки сборка такая же, как была.

## Как включить

```sh
cd ~/tmp/shennong
bash sideload/enable.sh      # build/trollstore + build/boot-bin/tsboot
bash build-ios.sh            # пересобирается только root.apfs, system.apfs не трогается
# дальше обычный запуск, boot-args те же (нужен var_apfs, он уже есть)
```

В логе сессии должно появиться:

```
tsboot: persona 99 made (got id 99): TrollStore can start its root helper now
tsboot: no TrollStore in /var/containers/Bundle/Application: installing it from /redwood/share/trollstore/TrollStore.tar
tsboot: trollstorehelper install-trollstore: exit 0
tsboot: done: TrollStore installed (...): its icon is on the home screen
```

На следующих загрузках будет `TrollStore installed: ...; re-registering its apps` и `refresh: exit 0`.

Выключить: `bash sideload/enable.sh off`. TrollStore и приложения на флеше остаются, их можно удалить из самого TrollStore.

## Как ставить .ipa на телефоне

* Safari: скачать .ipa, затем «Поделиться» → TrollStore. Или открыть ссылку
  `apple-magnifier://install?url=<прямая ссылка на .ipa>`, TrollStore скачает и поставит сам.
* Файлы: .ipa → «Поделиться» → TrollStore.
* В самом TrollStore: `+` → Install IPA File / Install from URL.

## Ограничения

* **Нужны расшифрованные .ipa.** FairPlay здесь не работает (`fairplayd.H2` сразу выходит,
  `src/inject_fairplay.c`), и TrollStore сам отказывает зашифрованным бинарникам («Cannot apply CoreTrust bypass on
  an encrypted binary»). Сайдлоад-сборки и дампы подходят, ipa прямо из App Store нет.
* Приложение должно запускаться на arm64 iOS 16.5 (система тут от iPhone X): смотрите MinimumOSVersion в .ipa.
* Если в .ipa бинарник без подписи, TrollStore нужен ldid: Settings → Install ldid (качает с GitHub, нужен Wi-Fi).

## Что проверено, а что нет

Проверено: ядро (xnu-8796 `sys_persona.c`, `kern_exec.c`) действительно пропускает `kpersona_alloc` и spawn
в persona по одному только entitlement. Подпись `tsboot` сверена: хэши страниц и слот entitlements совпадают.

На телефоне ещё не запускалось. Возможные места, где упрётся:
1. `install-trollstore` / `refresh` вызываются, пока lsd или containermanagerd ещё не готовы: `tsboot` повторяет 3 раза с паузой 30 с.
2. Приложение типа User с контейнером данных: здесь до сих пор запускались только приложения из
   `/Applications` (Sileo, TrollStore Helper, RE4) без контейнера. Если приложение, поставленное через TrollStore,
   не стартует, следующий шаг: `libredwood-app.dylib` в него (`INJECT`), он покажет последние слова процесса.
3. Сохранит ли lsd регистрации между загрузками. Если нет, их восстанавливает `refresh` из `tsboot`.
