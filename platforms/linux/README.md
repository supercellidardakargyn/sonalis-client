# Sonalis Linux

Ubuntu 22.04+/Fedora 40+ için GTK4, libsecret, PipeWire, libcurl ve ortak C++20
çekirdek kullanan yerel istemcidir. Web motoru yoktur.

Hesap yenileme, oda/kanal gezinmesi, şifreli mesaj modeli, realtime/voice grant,
Secret Service güvenli saklama, 48 kHz mono PipeWire capture/render, Opus ve leaf
sertifika pinli ChaCha20-Poly1305 UDP relay uygulanmıştır. PipeWire callback’leri
sabit tampon ve realtime stream bayrakları kullanır.

CI Release derlemesini test eder ve unsigned tar.gz bundle üretir. Gerçek PipeWire
hot-swap/NAT kabulü Linux ortamında tamamlanmadan paket stable sayılmaz.
Flatpak/AppImage imzası bu teslimin dışındadır.
