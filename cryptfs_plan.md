# План реализации Stackable Filesystem LKM (прозрачное шифрование)

## 🖥️ Рекомендации по гипервизору и дистрибутиву

### Гипервизор: QEMU/KVM (основной) + VirtualBox (запасной)

Документ упоминает "QEMU + GDB в режиме отладки ядра" — это золотой стандарт для разработки LKM.

| | QEMU/KVM | VirtualBox |
|---|---|---|
| Отладка ядра через GDB | ✅ `-s -S` флаги из коробки | ❌ сложно настроить |
| Снапшоты после kernel panic | ✅ | ✅ |
| Скорость запуска | ✅ быстро | ✅ |
| Простота установки | ⚠️ чуть сложнее | ✅ очень просто |
| Headless / CLI режим | ✅ идеален | ⚠️ |

**Итого:** если на хосте стоит Linux — QEMU/KVM. Если хост Windows — VirtualBox как наиболее простой вариант с хорошей поддержкой снапшотов.

### Дистрибутив: Ubuntu 22.04 LTS (ядро 5.15)

Почему именно он:
- Ядро 5.15 — достаточно свежее, все нужные API (`crypto_skcipher`, `vfs_kern_mount`, `iov_iter`) стабильны
- `linux-headers-$(uname -r)` ставится одной командой, out-of-tree сборка работает без танцев
- Огромная база примеров и Stack Overflow под Ubuntu
- Не берём 24.04 (ядро 6.8) — там поменялся ряд `address_space_operations` полей, придётся адаптировать код под новый API (например, `readpage` заменён на `read_folio`)
- Не берём Arch/Gentoo — нестабильное ядро мешает воспроизводимости

---

## Фаза 0 — Подготовка стенда (1–2 часа)

### 0.1 Установка VM

```bash
# Для VirtualBox: создать VM, 4 GB RAM, 40 GB диск, Ubuntu 22.04 LTS
# Важно: сделать снапшот СРАЗУ после чистой установки — это точка восстановления
```

### 0.2 Установка зависимостей

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) \
    gcc make git kmod vim gdb
```

### 0.3 Проверка окружения

```bash
uname -r          # должно быть 5.15.x
ls /lib/modules/$(uname -r)/build   # должен существовать
```

### 0.4 Настройка QEMU для отладки (опционально, но рекомендуется)

```bash
# Запуск гостевой системы с GDB-сервером:
qemu-system-x86_64 -kernel vmlinuz -s -S &
gdb vmlinux
(gdb) target remote :1234
(gdb) continue
```

Это позволит ставить брейкпоинты прямо в коде модуля при kernel panic вместо того, чтобы читать dmesg.

---

## Фаза 1 — Изучение эталонной реализации ECryptFS (2–4 часа)

### 1.1 Скачать исходники ядра и изучить ключевые файлы

```bash
git clone --depth=1 https://github.com/torvalds/linux
```

Изучить в следующем порядке:
- `fs/ecryptfs/main.c` — регистрация `file_system_type`, `mount()`
- `fs/ecryptfs/file.c` — реализация `read_iter` / `write_iter` поверх lower FS
- `fs/ecryptfs/inode.c` — создание upper inode, привязка к lower inode
- `fs/overlayfs/file.c` — более современный пример (OverlayFS)

### 1.2 Ключевые паттерны, которые нужно понять

- Как upper inode хранит ссылку на lower file (`ecryptfs_file_to_lower()`)
- Как вызывается `vfs_iter_read()` для чтения из нижней ФС
- Как работает `struct ecryptfs_crypt_stat` для хранения ключа на inode

---

## Фаза 2 — Skeleton модуля (4–6 часов)

### 2.1 Минимальный LKM без шифрования — только монтирование

Структура проекта:

```
cryptfs/
├── Makefile
├── cryptfs_main.c      # регистрация ФС, mount/umount
├── cryptfs_inode.c     # upper inode, inode_operations
├── cryptfs_file.c      # file_operations (read_iter, write_iter)
├── cryptfs_super.c     # superblock
└── cryptfs_crypto.c    # обёртка над kernel crypto API
```

### 2.2 Makefile

```makefile
obj-m += cryptfs.o
cryptfs-objs := cryptfs_main.o cryptfs_inode.o cryptfs_file.o \
                cryptfs_super.o cryptfs_crypto.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
```

### 2.3 Регистрация файловой системы (`cryptfs_main.c`)

```c
static struct file_system_type cryptfs_fs_type = {
    .owner      = THIS_MODULE,
    .name       = "cryptfs",
    .mount      = cryptfs_mount,
    .kill_sb    = kill_anon_super,
};

static int __init cryptfs_init(void) {
    return register_filesystem(&cryptfs_fs_type);
}

static void __exit cryptfs_exit(void) {
    unregister_filesystem(&cryptfs_fs_type);
}
```

### 2.4 Функция монтирования

```c
static struct dentry *cryptfs_mount(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *raw_data)
{
    // Монтируем нижнюю ФС
    struct vfsmount *lower_mnt = kern_mount_data(&ext4_fs_type, raw_data);
    // Создаём superblock поверх
    return mount_nodev(fs_type, flags, raw_data, cryptfs_fill_super);
}
```

### 2.5 Проверка skeleton без шифрования

```bash
sudo insmod cryptfs.ko
sudo mount -t cryptfs -o lowerdir=/mnt/lower /mnt/upper
ls /mnt/upper    # должны быть видны файлы из /mnt/lower
sudo umount /mnt/upper
sudo rmmod cryptfs
dmesg | tail -20  # проверка на ошибки
```

---

## Фаза 3 — Интеграция kernel crypto API (6–8 часов)

### 3.1 Инициализация шифра в `cryptfs_crypto.c`

```c
struct crypto_skcipher *tfm;
struct skcipher_request *req;

// Инициализация AES-CBC
tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
if (IS_ERR(tfm)) {
    pr_err("cryptfs: failed to alloc cipher\n");
    return PTR_ERR(tfm);
}

// Установка ключа (256 бит)
u8 key[32] = { /* hardcoded для прототипа */ };
crypto_skcipher_setkey(tfm, key, sizeof(key));
```

> ⚠️ **Важно:** для прототипа используем хардкоженный ключ. Управление ключами через ioctl — в фазе 5.

### 3.2 Реализация `read_iter` (`cryptfs_file.c`)

```c
static ssize_t cryptfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *lower_file = cryptfs_file_to_lower(iocb->ki_filp);

    // 1. Выделить временный буфер
    size_t len = iov_iter_count(to);
    void *buf = kmalloc(len, GFP_KERNEL);

    // 2. Прочитать зашифрованные данные из нижней ФС
    struct iov_iter lower_iter;
    iov_iter_kvec(&lower_iter, READ, &kvec, 1, len);
    vfs_iter_read(lower_file, &lower_iter, &iocb->ki_pos, 0);

    // 3. Дешифровать: crypto_skcipher_decrypt()
    cryptfs_decrypt(buf, len);

    // 4. Передать plaintext пользователю
    copy_to_iter(buf, len, to);

    kfree(buf);
    return len;
}
```

### 3.3 Реализация `write_iter` — симметрично

```c
static ssize_t cryptfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    // 1. Скопировать данные из userspace
    // 2. Зашифровать: crypto_skcipher_encrypt()
    // 3. Записать в нижнюю ФС через vfs_iter_write()
}
```

### 3.4 Выбор режима шифрования

Рекомендуется **AES-XTS** (как в dm-crypt), а не AES-CBC:
- AES-CBC требует padding, что усложняет работу с произвольными смещениями в файле
- AES-XTS работает с 512-байтными секторами без padding — лучше для файловых операций
- Строка инициализации: `"xts(aes)"`

---

## Фаза 4 — Обработка mmap (3–5 часов)

Это критически важная фаза, которую легко пропустить.

### 4.1 Проблема

Без переопределения `address_space_operations` любой `mmap(MAP_SHARED)` на файл обойдёт шифрование, обращаясь напрямую к page cache с зашифрованными данными.

### 4.2 Решение

```c
static const struct address_space_operations cryptfs_aops = {
    .read_folio     = cryptfs_read_folio,   // Ubuntu 22.04, ядро 5.15
    .writepage      = cryptfs_writepage,
    .write_begin    = cryptfs_write_begin,
    .write_end      = cryptfs_write_end,
};

static int cryptfs_read_folio(struct file *file, struct folio *folio)
{
    // Читаем страницу из нижней ФС и дешифруем перед помещением в cache
    struct page *page = &folio->page;
    // ... vfs_read lower + decrypt + set_page_uptodate
}
```

### 4.3 Привязка к inode

```c
// При создании upper inode:
inode->i_mapping->a_ops = &cryptfs_aops;
```

---

## Фаза 5 — Userspace-утилита управления ключами (3–4 часа)

### 5.1 Интерфейс через ioctl

Определение в заголовочном файле (shared между модулем и утилитой):

```c
#define CRYPTFS_IOC_MAGIC  'C'
#define CRYPTFS_IOC_SETKEY _IOW(CRYPTFS_IOC_MAGIC, 1, struct cryptfs_key_t)

struct cryptfs_key_t {
    uint8_t key[32];   // 256-bit AES key
    uint8_t iv[16];    // 128-bit IV
};
```

### 5.2 Обработчик в модуле

```c
static long cryptfs_ioctl(struct file *file, unsigned int cmd,
                           unsigned long arg)
{
    switch (cmd) {
    case CRYPTFS_IOC_SETKEY:
        copy_from_user(&stored_key, (void __user *)arg,
                       sizeof(struct cryptfs_key_t));
        crypto_skcipher_setkey(tfm, stored_key.key, 32);
        return 0;
    }
    return -EINVAL;
}
```

### 5.3 Утилита `cryptfs-ctl` (C, userspace)

```c
// cryptfs-ctl mount /dev/sdb1 /mnt/secret --key=<hex_key>
int main(int argc, char *argv[]) {
    int fd = open("/dev/cryptfs_ctl", O_WRONLY);
    struct cryptfs_key_t k = { /* parse from argv */ };
    ioctl(fd, CRYPTFS_IOC_SETKEY, &k);
    mount("cryptfs", argv[2], "cryptfs", 0,
          "lowerdir=/mnt/lower");
}
```

---

## Фаза 6 — Отладка (ongoing)

### 6.1 Инструменты

| Инструмент | Применение |
|---|---|
| `dmesg -w` | Следить за `pr_info`/`pr_err` в реальном времени |
| `ftrace` | Трассировка вызовов VFS-функций |
| `QEMU + GDB` | Брейкпоинты в коде модуля, анализ kernel panic |
| `kasan` | Детектор use-after-free и out-of-bounds в ядре |

### 6.2 Включение KASAN

Пересобрать ядро гостя с `CONFIG_KASAN=y` — опционально, но сильно помогает при работе с буферами.

### 6.3 Типичные ловушки

- **Deadlock в `write_iter`**: нельзя вызывать `vfs_iter_write()` под `i_mutex` нижнего inode, если его уже держит caller
- **Утечка памяти**: всегда `kfree(buf)` в error path через `goto out`
- **Неправильный `ki_pos`**: после вызова lower read/write позиция сдвигается, надо восстанавливать для upper inode вручную

---

## Фаза 7 — Финальное тестирование

```bash
# 1. Базовое шифрование/дешифрование
echo "secret data" > /mnt/cryptfs/test.txt
cat /mnt/cryptfs/test.txt          # должно вернуть "secret data"
xxd /mnt/lower/test.txt            # должны быть зашифрованные байты

# 2. Тест mmap
python3 -c "
import mmap, os
f = open('/mnt/cryptfs/test.txt', 'r+b')
m = mmap.mmap(f.fileno(), 0)
print(m[:11])   # должно быть b'secret data'
"

# 3. Стресс-тест
dd if=/dev/urandom of=/mnt/cryptfs/big.bin bs=1M count=100
md5sum /mnt/cryptfs/big.bin        # запомнить
umount /mnt/cryptfs && mount ...   # перемонтировать
md5sum /mnt/cryptfs/big.bin        # должно совпасть
```

---

## Итоговый порядок снапшотов VM

| Снапшот | Состояние |
|---|---|
| **Снапшот 0** | Чистая Ubuntu 22.04, только `apt update` |
| **Снапшот 1** | Установлены все зависимости, скачан ECryptFS |
| **Снапшот 2** | Skeleton компилируется и монтируется |
| **Снапшот 3** | `read`/`write` с шифрованием работают |
| **Снапшот 4** | `mmap` обработан |

Каждый kernel panic → откат к предыдущему снапшоту, без переустановки системы.