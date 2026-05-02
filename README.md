# cryptfs — Stackable FS with transparent AES-256-XTS encryption

Реализация stackable-файловой системы по плану из `cryptfs_plan.md`.

> **Целевое ядро — Linux 5.15 LTS** (Ubuntu 22.04). Код опирается на
> API этого ядра: `fs_context` + `get_tree_nodev`, в inode_operations —
> `struct user_namespace *mnt_userns` (параметр `&init_user_ns`),
> `generic_fillattr(userns, inode, stat)` без `request_mask`,
> поля времени inode читаются/пишутся прямым доступом к
> `inode->i_[acm]time`.

## Сборка

```bash
make
```

Используется системный `gcc` (на Ubuntu 22.04 это gcc-11, которым
собрано и само ядро 5.15 — версии компилятора совпадают, никаких
обёрток не требуется).

После сборки на диске:
- `cryptfs.ko` — kernel-модуль
- `cryptfs-ctl` — userspace-утилита для управления ключом

## Загрузка

```bash
sudo insmod cryptfs.ko
dmesg | tail
# ожидается:
#   cryptfs: crypto initialized (xts(aes), default key)
#   cryptfs: /dev/cryptfs_ctl registered
#   cryptfs: loaded
```

Появляется:
- зарегистрированная ФС `cryptfs` в `/proc/filesystems`
- управляющее устройство `/dev/cryptfs_ctl`

## Монтирование

```bash
sudo mkdir -p /mnt/lower /mnt/upper
sudo mount -t cryptfs -o lowerdir=/mnt/lower none /mnt/upper
```

Все чтения/записи через `/mnt/upper/...` прозрачно шифруются
AES-256-XTS. Шифртекст хранится в `/mnt/lower/`.

## Смена ключа

Модуль стартует с тестового «нулевого» ключа. В реальной работе:

```bash
KEY=$(./cryptfs-ctl genkey)
sudo ./cryptfs-ctl setkey "$KEY"
```

> **Важно:** ключ — глобальный для всех mount-точек cryptfs. Менять
> ключ в момент активных I/O не следует.

## Проверка

```bash
echo "hello, cryptfs" | sudo tee /mnt/upper/test.txt
sudo cat /mnt/upper/test.txt            # => "hello, cryptfs\n" + нули до 512 байт
sudo xxd /mnt/lower/test.txt | head     # => случайно выглядящий шифртекст
```

## Размонтирование и выгрузка

```bash
sudo umount /mnt/upper
sudo rmmod cryptfs
```


## Структура исходников

| Файл                | Назначение                                       |
|---------------------|--------------------------------------------------|
| `cryptfs_main.c`    | module init/exit, регистрация `file_system_type` |
| `cryptfs_super.c`   | `fs_context`, разбор опций, `fill_super`, sops   |
| `cryptfs_inode.c`   | upper-inode lifecycle + `inode_operations`       |
| `cryptfs_file.c`    | `read_iter` / `write_iter` с шифрованием         |
| `cryptfs_crypto.c`  | обёртка над kernel crypto API (`xts(aes)`)       |
| `cryptfs_ctl.c`     | `/dev/cryptfs_ctl` — ioctl для ключа             |
| `cryptfs-ctl.c`     | userspace-утилита                                |
| `cryptfs.h`         | внутренний заголовок модуля                      |
| `cryptfs_uapi.h`    | общий заголовок userspace/kernel (ioctl)         |
                                          

1. Загрузка модуля

  cd ~/our_driver
  sudo insmod cryptfs.ko                                                        
  dmesg | tail -5
  # ожидается:                                                                  
  #   cryptfs: crypto initialized (xts(aes), default key)                       
  #   cryptfs: /dev/cryptfs_ctl registered                                      
  #   cryptfs: loaded                                                           
                                                                                
  lsmod | grep cryptfs                                                          
  grep cryptfs /proc/filesystems    # => "nodev  cryptfs"   
  ls -l /dev/cryptfs_ctl            # crw------- root root                      
                                                                                
2. Подготовка точек монтирования                                              
                                                                                
  sudo mkdir -p /mnt/lower /mnt/upper                                           
  # нижняя ФС может быть любой (tmpfs/ext4/…). Для теста — tmpfs:
  sudo mount -t tmpfs none /mnt/lower                                           
                                                            
3. Монтирование cryptfs поверх                                                
                                                            
  sudo mount -t cryptfs -o lowerdir=/mnt/lower none /mnt/upper                  
  mount | grep cryptfs                                                          
  dmesg | tail -3
  # => cryptfs: mounted over 'lower'                                            
                                                                                
4. Тест шифрования                                                 
                                                                                
  Вот ваш сценарий — ключевой момент в том, что шифртекст смотрим напрямую в    
  lowerdir, в обход драйвера. Отключение модуля для этого не требуется (и даже
  вредно — umount на всякий случай делаем сначала), но ниже покажу и полный цикл
   с перезагрузкой модуля.                                  

  # пишем plaintext через cryptfs:
  echo "hello, cryptfs — secret payload" | sudo tee /mnt/upper/test.txt         
                                                                                
  # читаем обратно через cryptfs — должен выйти исходный текст                  
  #   (плюс нули до границы 512-байтного сектора):                                                                         
  sudo cat /mnt/upper/test.txt                              
                                                                                
  # а теперь смотрим сырой файл в lowerdir — это шифртекст:                   
  sudo xxd /mnt/lower/test.txt | head
  sudo ls -la /mnt/lower/test.txt   # размер = 512 (один сектор XTS)            
                                                                                
  В xxd /mnt/lower/test.txt должны быть случайно выглядящие байты — там         
  AES-256-XTS шифртекст, на диске нигде слова secret нет.                       
                                                                                
5. Отключение драйвера и повторный тест                   
                 
  sudo umount /mnt/upper                                                        
  sudo rmmod cryptfs                                        
  dmesg | tail -3     # ожидается "cryptfs: unloaded"

  # теперь cryptfs уже не смонтирован, смотрим файл без драйвера: 
  sudo xxd /mnt/lower/test.txt | head      # шифртекст
                                                                              
 Для полного очищения 
  sudo umount /mnt/lower                                                                    
   
 
                                                                                
Что делать, если insmod упадёт                                                
                                                                                
  - Invalid module format / version magic — почти всегда uname -r не совпадает с
   версией, под которую собрали. make clean && make после sudo apt install 
  linux-headers-$(uname -r).                                                    
  - Любой kernel panic/oops — dmesg даст стек, присылайте его сюда, разберём.
  Oops от модуля не обязательно ронит систему, но rmmod после этого часто уже не
   пройдёт — понадобится ребут.
                                                                                
