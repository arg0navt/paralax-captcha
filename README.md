# Paralax Captcha

## Сборка

```sh
zig cc -target x86_64-windows-msvc src/main.c src/renderer/renderer.c
  -Iinclude -Isrc/renderer -Ilib/webp/include
  -D_CRT_SECURE_NO_WARNINGS
  lib/webp/lib/libwebp.lib -o build/paralax-captcha.exe
```

## Запуск

```sh
zig build run
```

