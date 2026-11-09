# Kullanım Kılavuzu

Apartman zili ve kapı açma sistemi. Bu kılavuzdaki süreler ve şifre uzunlukları **varsayılan** değerlerdir;
kurulumda farklı ayarlanmış olabilir (bkz. [Yapılandırma Ayarları](yapilandirma-ayarlari.md)).

---

## 1. Sistem

| Cihaz | Yeri | Görevi |
| :--- | :--- | :--- |
| **Panel** | Bina girişi (binada bir tane) | Tuş takımı, kapı kilidi rölesi, durum LED'i |
| **Daire modülü** | Her dairede bir tane | Zil rölesi, kapı açma butonu, gizli eşleştirme ve sıfırlama butonları, durum LED'i |

- Cihazlar bina içindeki **erişim noktası (Access Point — AP)** üzerinden kablosuz (Wi-Fi / UDP) haberleşir. Çok katlı
  binalarda kapsama alanını genişletmek için aynı ağ adını (SSID) ve şifresini yayınlayan kablolu erişim noktaları veya
  köprü (bridge) modunda çalışan sinyal güçlendiriciler kullanılabilir.
- **Bina erişim noktası (AP) gereksinimleri:** 2,4 GHz bandında görünür SSID (kanal 1, 6 veya 11), WPA2/WPA3 şifreleme,
  DHCP sunucusu açık (daire sayısı kadar IP havuzu ile), **istemci izolasyonu (client/AP isolation) kapalı** ve
  **802.11b hızları açık** (erişim noktasında "802.11b'yi kapat" ayarı kapalı) olmalıdır.
- Sistem yerel ağda kapalıdır: internete veya buluta bağlanmaz.
- Standart enerji varken açılan (fail-secure) elektrikli kilit karşılığı kullanıldığında, elektrik kesildiğinde kapı
  kilitli kalır.

---

## 2. Ziyaretçi: zil çalma

Panelde daire numarasını yazıp `#` tuşuna basın:

```text
12 #
```

| Panel LED'i | Anlamı |
| :--- | :--- |
| 2 saniye sabit yanar | Zil çaldı |
| 3 saniye orta hızda yanıp söner | Daire kayıtlı ama modül cevap vermedi |
| 1 saniye hızlı yanıp söner | Geçersiz numara veya bu numarada kayıtlı daire yok |

- Daire numaraları 1–99 arasıdır (başında sıfır olmadan yazılır).
- Zil, yanlış şifre kilidi sırasında da çalışır.

---

## 3. Sakin: kapıyı açma

### 3.1. Daireden
Daire modülündeki **kapı açma butonuna** basıp bırakın. Kapı kilidi 0,5 saniye açılır, paneldeki LED 0,5 saniye sabit
yanar.

| Modül LED'i | Anlamı |
| :--- | :--- |
| 2 saniye tam parlak yanar | Kapı açıldı |
| 3 saniye orta hızda yanıp söner | Panel cevap vermedi (kapı açılmadı) |

- Buton bırakıldığında çalışır.
- 0,1 saniyeden kısa veya 30 saniyeden uzun basışlar dikkate alınmaz (parazit veya sıkışmış buton).

### 3.2. Panelden şifreyle
```text
* <kullanıcı şifresi> #        Örnek: * 1234 #
```
- Kullanıcı şifresi binanın tamamı için tektir (4 hane; yönetici şifresiyle aynı olamaz).
- Kapı açıkken (0,5 saniye) gelen ikinci açma komutu yok sayılır.

### 3.3. Yazdığınızı iptal etme
```text
* #
```
Hiç tuşa basılmazsa yazılan 10 saniye sonra kendiliğinden silinir.

---

## 4. Yönetici işlemleri

Yönetici şifresi 8 hanedir. Tüm komutlar `*` ile başlar, `#` ile biter.

| İşlem | Komut |
| :--- | :--- |
| Kullanıcı şifresini değiştirme | `* <yönetici> * 3 * <yeni 4 hane> * <yeni 4 hane> #` |
| Yönetici şifresini değiştirme | `* <yönetici> * 4 * <yeni 8 hane> * <yeni 8 hane> #` |
| Daire eşleştirme | `* <yönetici> * 1 * <daire no> #` (bölüm 5) |
| Daire kaydını silme | `* <yönetici> * 2 * <daire no> * <daire no> #` |
| Bina Wi-Fi ayar portalını açma | `* <yönetici> * 5 #` (bölüm 4.2) |
| Bina sıfırlama | `* <yönetici> * 6 * <yönetici> #` (bölüm 6.3) |

- Başarılı işlemde panel LED'i 2 saniye sabit yanar; hatalı komutta 1 saniye hızlı yanıp söner.
- **Fabrika şifreleri** her cihaza özel olarak rastgele üretilir ve cihaz etiketinde yazar. Kurulumdan sonra ikisini de
  değiştirin.

### 4.1. Yanlış şifre kilidi
- Üst üste 5 yanlış şifreden sonra `*` ile başlayan girişler 60 saniye kilitlenir. LED seyrek tek flaşlar yapar.
- Hatalar sürerse kilit süresi her seferinde iki katına çıkar (120, 240 saniye …), en fazla 15 dakika.
- Doğru şifre girilince kilit seviyesi sıfırlanır.
- Kilit sırasında zil çalınabilir.
- Yanlış biçimde yazılmış giriş (örneğin `**#` ya da `* 1234 * #`) hata gösterir ama yanlış şifre sayılmaz.

### 4.2. Bina Wi-Fi ağını ayarlama (portal)
Bina erişim noktasının (AP) Wi-Fi adı (SSID) ve şifresi kurulumdan önce belirlenmiş olur. Bu bilgiyi firmware'i yeniden
derlemeden panelde değiştirmek için:

1. Panelde `* <yönetici> * 5 #` yazın. Panel LED'i 2 saniye sabit yanar, ardından portal açık kaldığı sürece (**2
   dakika**) yavaş (1 Hz) yanıp söner ve panel `Doorbell-Setup` adlı Wi-Fi ağını açar. (İptal etmek için `* #`
   tuşlayabilirsiniz.)
2. Telefon veya bilgisayarla `Doorbell-Setup` ağına bağlanın. Bu ağın WPA3 şifresi, o anki **yönetici şifresidir**.
3. Tarayıcıda `http://192.168.4.1` adresini açın (bina ağı zaten `192.168.4.x` bloğunu kullanıyorsa `http://10.195.4.1`
   açılır), bina erişim noktasının Wi-Fi adını (SSID) ve şifresini girip kaydedin.
4. Panel yeni bilgilerle bina erişim noktasına bağlanmayı dener:
   - **Bağlantı başarılı olursa:** Yeni ayarlar kaydedilir, panel LED'i 2 saniye sabit yanar ve kurulum ağı kapanır.
   - **Bağlantı başarısız olursa (ör. şifre yanlışsa):** Eski ayarlar korunur, web sayfasında hata gösterilir ve panel
     LED'i 1 saniye hızlı yanıp söner.
   - Bina erişim noktası farklı bir Wi-Fi kanalındaysa, panel bağlanmayı denerken telefonunuzun `Doorbell-Setup`
     bağlantısı kısa süre kopabilir ve sonuç sayfası görünmeyebilir. Bu durumda sonucu **panel LED'inden** takip edin
     (2 saniye sabit = başarılı, 1 saniye hızlı yanıp sönme = başarısız). Başarısız olduysa ağa yeniden bağlanıp
     tekrar deneyin.

- Bina erişim noktasının Wi-Fi adı veya şifresi sonradan değiştirilirse, mevcut daire modüllerinin yeni ağ bilgisini
  alması için sıfırlanıp yeniden eşleştirilmesi gerekir (veya yeni erişim noktasına eski Wi-Fi adı ve şifresi verilir).
- Wi-Fi bilgileri sahada panelin seri port konsolundan (UART) da güncellenebilir.

---

## 5. Kurulum: daire modülünü eşleştirme

Eşleştirme **panelin yanında** yapılır; modül daireye eşleştirmeden sonra takılır. Eşleştirmeden önce panelin bina
erişim noktasına (AP) bağlı olduğundan (panel LED'inin sönük/hazır durumda olduğundan) emin olun.

1. **Panelde:** `* <yönetici> * 1 * <daire no> #` yazın.
   Panel LED'i tekrarlayan çift flaş yapar; 20 saniyelik eşleştirme penceresi açılır (iptal için `* #`).
2. **Modülde (panelin yanında):** Gizli **eşleştirme butonunu** basılı tutun.
   - 7. saniyede LED orta hızda yanıp sönmeye başlar. **Bu sırada bırakın** (7–12 saniye arası).
   - 12 saniyeyi geçerse LED söner ve bırakmak işe yaramaz; 15 saniyede LED üçlü flaş yapar ve işlem iptal olur.
   - Bıraktığınızda LED yavaş (1 Hz) yanıp söner: modül panelin eşleştirme ağına (`Doorbell-Pair-XXXX`) bağlanıp bina
     Wi-Fi bilgisini ve şifreleme anahtarını alır, ardından bina erişim noktasına (AP) bağlanarak eşleştirmeyi
     doğrular.
3. **Sonuç:**

| Durum | Panel | Modül |
| :--- | :--- | :--- |
| Başarılı | LED 2 saniye sabit yanar | LED 2 saniye tam parlak yanar, zil bir kez çalar, sonra LED loş sabit kalır |
| Birden fazla eşleşmemiş modül bulundu | LED 1 saniye hızlı yanıp söner; eşleştirme iptal | Pencere bitince eşleşmemiş duruma döner |
| Modül bulunamadı veya bina ağına bağlanamadı | LED 1 saniye hızlı yanıp söner | Eşleşmemiş duruma döner |
| Daire numarası dolu veya panel ağa bağlı değil | LED 1 saniye hızlı yanıp söner | — |

4. **Etiketleme ve dairede doğrulama:** Modülün üzerine eşleştirildiği **daire numarasını** yazın ve modülü daireye
   takın.
   - Modül bina ağına bağlandığında LED'i **loş sabit** yanar (kapsama alanı dışındaysa seyrek tek flaş yapar).
   - Dairedeki **kapı açma butonuna** bir kez basarak kapının açıldığını (modül LED'inin 2 saniye tam parlak yandığını)
     ve panelden `<daire no> #` tuşlayarak doğru dairenin zilinin çaldığını doğrulayın.

**Kurallar:**
- Dolu bir daire numarasının üzerine yazılmaz; önce o kaydı silin.
- Eşleşmiş bir modül, sıfırlanmadan tekrar eşleştirilemez (bölüm 6.2; eşleşmiş modülde eşleştirme butonuna basılırsa
  üçlü flaş ile reddedilir).
- Aynı anda yalnızca **bir** modül eşleştirme modunda olmalıdır.

---

## 6. Sıfırlama

### 6.1. Panel şifre sıfırlama
Panel kutusunun içindeki gizli butonu **10–60 saniye** basılı tutup bırakın (10. saniyede LED sabit yanmaya başlar,
arada kısa söner; bu sırada bırakın). Değiştirilmiş şifreler silinir; panel etiketteki fabrika şifrelerine döner. Daire
kayıtları ve bina Wi-Fi bilgileri korunur.

### 6.2. Daire modülü sıfırlama
Modüldeki gizli **sıfırlama butonunu** **7–12 saniye** basılı tutup bırakın.
- 7. saniyede LED sabit yanmaya başlar (arada kısa söner). **Bu sırada bırakın** (7–12 saniye arası); 15 saniyede
  LED üçlü flaş yapar ve işlem iptal olur.
- Modülün eşleştirme ve Wi-Fi bilgileri silinir, modül yeniden başlar ve eşleşmemiş duruma döner.
- Paneldeki daire kaydı silinmez. Aynı daire numarasıyla yeniden eşleştirmek için önce panelden kaydı silin
  (bölüm 4). Modülü **başka** bir numarayla eşleştirirseniz eski kaydı yeni eşleştirme tamamlandığında otomatik olarak
  silinir.

### 6.3. Bina sıfırlama
```text
* <yönetici> * 6 * <yönetici> #
```
Tüm daire kayıtları ve şifreleme anahtarları silinir; şifreler ve bina Wi-Fi ağı bilgileri korunur. Panel yeniden
başlar. **Bütün daire modülleri sıfırlanıp yeniden eşleştirilmelidir.**

### 6.4. Yeniden başlatma (hiçbir şey silinmez)
- Cihaz kilitlenirse kendini yeniden başlatır; ne olduğu panelin monitöründe görünür ("last restart: task watchdog"
  gibi).
- Panel uzun süre hiçbir modülden rapor alamazsa (varsayılan 15 dakika) kendini yeniden başlatır. Durum sürerse bekleme
  süresi her seferinde iki katına çıkar (30 dakika, 1 saat … en fazla 48 saat).
- Elle yeniden başlatmak için kart üzerindeki **EN** butonuna basın ya da elektriğini kesip açın. Kutunun içine EN ile
  GND arasına gizli bir buton da bağlanabilir (ESP32'de EN butonu ve elektrik kesintisi raporda `POWERON` olarak
  görünür). Kayıtlar, şifreler, kilit durumu, Wi-Fi ayarları ve eşleştirme bilgileri korunur.

### 6.5. Panelin değiştirilmesi
Daire kayıtları ve şifreleme anahtarları yalnızca panelde saklanır; bu bilgilerin yedeği alınamaz. Panel arızalanıp
yenisiyle değiştirilirse (veya panelin hafızası silinirse):

1. Yeni panelde bina Wi-Fi ağını ayarlayın (bölüm 4.2) ve fabrika şifrelerini değiştirin (bölüm 4).
2. **Bütün daire modüllerini** sıfırlayın (bölüm 6.2).
3. Her modülü yeni panelle yeniden eşleştirin (bölüm 5).

Eski panele eşleştirilmiş modüller yeni panelle çalışmaz; panelden cevap alamadıkları için yaklaşık 15 dakika içinde
seyrek tek flaş yapmaya başlarlar ve sıfırlanıp yeniden eşleştirilene kadar böyle kalırlar.

---

## 7. LED durumları

Birden fazla durum aynı anda oluşursa üstteki (öncelikli) durum gösterilir; geçici gösterim bittiğinde LED bir önceki
sürekli durumuna döner.

### 7.1. Panel
| LED | Anlamı |
| :--- | :--- |
| Sönük | Beklemede, ağa bağlı ve hazır |
| Tek kısa flaş | Tuşa basıldı |
| Yavaş yanıp sönme (1 Hz) | Komut yazılıyor veya Wi-Fi ayar portalı açık (2 dakika) |
| 2 saniye sabit | Başarılı / zil çaldı / eşleşti / Wi-Fi portalı başlatıldı |
| Kapı açılırken sabit (varsayılan 0,5 saniye) | Kapı açıldı |
| 1 saniye hızlı yanıp sönme | Hata / yanlış şifre / reddedildi / daire dolu / birden fazla modül |
| 3 saniye orta hızda yanıp sönme | Daire kayıtlı ama cevap vermiyor |
| Sabit, arada kısa sönme | Şifre sıfırlama butonu basılı (10–60 sn): şimdi bırakın |
| Tekrarlayan çift flaş | 20 saniyelik eşleştirme penceresi açık |
| Seyrek tek flaşlar | Tuş takımı yanlış şifre nedeniyle kilitli veya panelin bina Wi-Fi ağı bağlantısı kopuk |

### 7.2. Daire modülü
| LED | Anlamı |
| :--- | :--- |
| Tekrarlayan çift flaş | Eşleşmemiş |
| Seyrek tek flaşlar | Eşleşmiş ama ağ bağlantısı yok, kapsama alanı dışında veya panele ulaşılamıyor (kayıt silinmiş / panel kapalı) |
| Loş, sabit | Normal, eşleşmiş ve ağa bağlı |
| Hızlı yanıp sönme | Zil çalıyor (görsel zil — en yüksek önceliklidir) |
| 2 saniye tam parlak | Kapı açıldı / eşleştirme başarılı |
| 3 saniye orta hızda yanıp sönme | Kapı açılamadı (panel cevap vermiyor) |
| Orta hızda yanıp sönme | Eşleştirme butonu basılı (7–12 sn): şimdi bırakın |
| Yavaş yanıp sönme (1 Hz) | Eşleştirme modu: panel aranıyor ve ağa bağlanılıyor |
| Sabit, arada kısa sönme | Sıfırlama butonu basılı (7–12 sn): şimdi bırakın |
| Tekrarlayan üçlü flaş | Buton 15 saniyeden uzun basıldı veya geçersiz buton basımı; iptal |

---

## 8. Sorun giderme

Panelin seri monitörü (UART, varsayılan `115200` baud) tüm daireleri tek yerden gösterir: her modül belirli aralıklarla
panele bir durum raporu gönderir (çalışma süresi, son yeniden başlama nedeni, cevapsız kapı açma istekleri, reddedilen
mesajlar, bağlı olduğu erişim noktasından alınan sinyal gücü ve kanal bilgisi). Bir modülden uzun süre rapor gelmezse
panel bunu uyarı olarak yazar (bkz. Yapılandırma Ayarları, heartbeat).

| Belirti | Olası neden | Ne yapmalı |
| :--- | :--- | :--- |
| Zil çalınca panel 3 saniye orta hızda yanıp sönüyor | Modülün elektriği yok, erişim noktası (AP) / sinyal güçlendirici (repeater) kapalı veya sinyal ulaşmıyor | Modülün, erişim noktasının (AP) ve varsa sinyal güçlendiricilerin açık olduğundan emin olun |
| Zil çalınca panel 1 saniye hızlı yanıp sönüyor | Bu numarada kayıtlı daire yok | Numarayı kontrol edin; gerekiyorsa daireyi eşleştirin |
| Eşleştirme olmuyor | Modül zaten eşleşmiş, 20 saniye doldu, başka bir modül de eşleştirme modunda ya da panel bina erişim noktasına bağlı değil | Panelin bina Wi-Fi ağına bağlı olduğundan emin olun (4.2); modülü sıfırlayın (6.2), diğer modülleri eşleştirme modundan çıkarın, tekrar deneyin |
| Tuş takımı şifre kabul etmiyor, LED seyrek flaş yapıyor | Yanlış şifre kilidi | Kilit süresinin bitmesini bekleyin |
| Şifreler unutuldu | — | Panel şifre sıfırlama (6.1); etiketteki fabrika şifreleri geçerli olur |
| Modül sürekli çift flaş yapıyor | Modül eşleşmemiş | Eşleştirin (bölüm 5) |
| Modül seyrek tek flaş yapıyor | Modül kapsama alanı dışında, erişim noktası (AP) / panel kapalı ya da dairenin kaydı panelden silinmiş | Erişim noktasını ve paneli kontrol edin; daire kaydı silindiyse modülü sıfırlayıp (6.2) yeniden eşleştirin |
| Kapı açma butonuna basınca modül 3 saniye orta hızda yanıp sönüyor | Panel kapalı veya ağ bağlantısı kopuk | Panelin enerjisini ve erişim noktası (AP) bağlantısını kontrol edin |
