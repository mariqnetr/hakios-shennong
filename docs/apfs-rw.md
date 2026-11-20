# APFS с записью: рабочая папка

Стенд на Mac: наш драйвер (build/hackios/xnu/files/bsd/miscfs/apfs) собирается программой и пишет в файл-образ;
судья — Apple: `fsck_apfs -n IMAGE` и монтирование в macOS.

Пустой контейнер от Apple (1 ГБ):
    mkfile -n 1g t.img; D=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage t.img | awk '{print $1}')
    newfs_apfs -v Data $D; hdiutil detach $D

Сборка (src = копия miscfs/apfs + apfs_txn.[ch], в apfs_lib.h добавлен dev.sync):
    O=/opt/homebrew/opt/openssl; clang -O1 -g -I src -I $O/include txntest.c hostdefl.c src/apfs_txn.c src/apfs_lib.c \
      src/libdeflate/lib/*.c src/libzbitmap/libzbitmap.c -L $O/lib -lcrypto -lz -o txntest

Сделано: apfs_txn.c — чекпоинт (эфемерные объекты в кольцо данных, карта, суперблок, блок 0).
500 пустых транзакций с обёртыванием колец и обрыв питания на каждой записи: fsck_apfs OK, читатель берёт xid до/после.
Дальше: spaceman (CIB, битмапы, internal pool), CoW B-деревьев, omap, затем операции ФС.

2б (сделано): apfs_space.c — CIB, битмапы чанков, internal pool и его кольцо битмапов. Освобождённое в транзакции
свободно в её же битмапах и не выделяется до её коммита; очереди free queue опустошаются при загрузке.
Проверено fsck_apfs: drain, alloc+free, утечка 100 блоков ловится точно (0x426+100), освобождение, выделение
через границу чанка, 40 коммитов (кольцо IP-битмапов 16), обрыв на каждой записи коммита.
Сборка теста: ... sptest.c src/apfs_space.c src/apfs_txn.c src/apfs_lib.c ... -o sptest

2в (сделано): apfs_bt.c (B-деревья с CoW: физические и виртуальные, разбиение, удаление, оглавление как у Apple,
ключ родителя = первый ключ ребёнка), apfs_vw.c (коммит: дерево ФС → omap тома → omap_phys → суперблок тома →
omap контейнера → суперблок контейнера). 3 (начато): apfs_fsw.c — создание файла/каталога.
5000 файлов + 3 каталога, коммит каждые 500: fsck_apfs чисто, macOS монтирует и видит все.
Исходники теперь в src/ (копия miscfs/apfs из hackios нужна рядом: apfs_lib.c, apfs_format.h, libdeflate, libzbitmap;
apfs_lib.h здесь — с полем dev.sync). Сборка теста: bash build.sh mktest.c

3б (сделано): apfs_fsw.c — запись (на месте и в новые экстенты, дыры как у Apple — экстент с блоком 0), truncate,
удаление файла/каталога, переименование (с заменой цели), chmod/chown/времена/флаги, xattr (встроенные, до 3804 байт),
симлинки; дерево ссылок на экстенты (extentref) ведётся. opstest: fsck_apfs чисто; macOS монтирует, f1 байт в байт
совпадает с ожидаемым, дыра, симлинк, xattr, переименование, права — всё как записано.
Не сделано: жёсткие ссылки, xattr больше 3804 байт (потоком), заполнение дыры внутри файла.
Дальше: vnops в ядре.

TODO (5 нояб. 2026): настоящий flush. flush_blocks прошивки занимает около 28 мкс: похоже, команда до UFS не доходит (вывод, не проверено).
Послать SYNCHRONIZE CACHE из загрузчика и проверить по времени. До этого барьеры коммита не гарантируют порядок на самом флеше.
