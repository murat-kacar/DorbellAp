# DorbellAp — Saha Kurulum, Kullanım ve Bakım Kılavuzu

Bu doküman sahada **Kurulum (Kurucu / Teknisyen)**, **Günlük Kullanım (Bina Sakini / Ziyaretçi)** ve **Yönetim, Bakım ve Sıfırlama (Yönetici / Teknisyen)** işlemleri için normatif kılavuzdur.

> Tüm süreler ve şifre uzunlukları fabrika varsayılanlarıdır. Derleme profiline veya özel yapılandırmaya göre değişiklikler için bkz. [Yapılandırma Ayarları](yapilandirma-ayarlari.md).

## Komut Söz Dizimi ve Gösterim Kuralları

Zil araması dışındaki tüm komutlar `*` ile başlar; tüm girişler `#` ile biter.

| Sembol / Kısaltma | Anlamı | Varsayılan Değer |
| :--- | :--- | :--- |
| `<daire no>` | Başında sıfır olmadan daire numarası | `1`–`99` |
| `<kullanıcı şifresi>` | Bina geneli ortak kapı açma şifresi | 4 haneli rakam |
| `<yönetici>` | Kurulum ve yönetim komut şifresi (aynı zamanda `Doorbell-Setup` Wi-Fi şifresi) | 8 haneli rakam |
| **Beklenen LED** | İşlemin başarılı/hatalı tamamlandığını doğrulayan gösterge | Bölüm 4 tabloları |

- **Önemli:** Yönetici ve kullanıcı fabrika şifreleri her cihaza özel olarak cihaz içi etikette basılıdır. Kurulum sırasında ikisini de değiştirin.

---

## 1. Kurulum (Kurucu / Teknisyen)

### 1.1. Ön Koşullar ve Bina Erişim Noktası (AP) Kontrol Listesi

Cihazlar bina içindeki yerel erişim noktası (AP) üzerinden Wi-Fi/UDP ile haberleşir; internete bağlanmaz. Kuruluma başlamadan önce aşağıdaki koşulları doğrulayın:

1. **Radyo ve Kanal:** 2,4 GHz bandında **görünür SSID** yayınlanıyor olmalıdır (önerilen sabit kanallar: **1, 6 veya 11**; kanal 12–13 kullanılacaksa cihaz ülke kodu `TR` olmalıdır).
2. **Güvenlik:** WPA2-PSK (AES) veya WPA2/WPA3 geçiş modu kullanın (WEP ve TKIP kabul edilmez).
3. **Ağ İzolasyonu ve Filtreler:** **İstemci izolasyonu (AP / Client Isolation)**, broadcast/multicast filtreleme ve **proxy-ARP** **kapalı** olmalıdır.
4. **DHCP Havuzu:** DHCP sunucusu açık olmalı ve IP havuzu `daire sayısı + 1 (panel) + altyapı` cihazı kapsayacak boyutta olmalıdır (240'tan fazla cihaz varsa `/23` alt ağı kullanın). Panel MAC adresi için sabit DHCP IP ayrılması önerilir.
5. **Çok Katlı Binalar (Çoklu AP / Güçlendirici):** En güvenilir topoloji aynı SSID/şifreyi yayınlayan **kablolu erişim noktalarıdır**. Kablosuz tekrarlayıcı (repeater) zorunluysa aynı SSID ve şifreyle, NAT veya `_EXT` uzantısı olmadan **köprü (Layer-2 bridge) modunda** ve en fazla **1 kablosuz aktarma (hop)** olacak şekilde kurun.
6. **Elektrikli Kilit Tipi:** Enerji verildiğinde açılan (fail-secure) elektrikli kilit karşılığı kullanın; elektrik kesildiğinde kapı kilitli kalır.

### 1.2. Panel İlk Kurulumu

Panel üzerinde bina Wi-Fi bilgileri flaşlama aşamasında yüklenmemişse cihaz açılışta ağa bağlanamaz ve panel LED'i **seyrek tek flaş (3 s arayla)** verir. Paneli bina ağına bağlamak için:

1. Panele enerji verin. (Bina Wi-Fi bilgileri flaşlama sırasında zaten tanımlandıysa ve panel ağa bağlandıysa LED **sönük** duruma geçer; doğrudan Adım 5'e atlayın.)
2. Panel tuş takımında kurulum portalını açın:

   ```text
   * <yönetici> * 5 #
   ```

   - **Beklenen LED:** Önce **2 saniye sabit**, ardından **yavaş yanıp sönme (1 Hz)**.
   - Panel 2 dakika süreyle `Doorbell-Setup` adlı geçici WPA3 Wi-Fi ağını açar. (İptal etmek için `* #` tuşlayın.)
3. Telefon veya bilgisayarla `Doorbell-Setup` ağına bağlanın. Wi-Fi parolası o anki `<yönetici>` şifresidir.
   - Çoğu telefonda kurulum sayfası otomatik açılır. Açılmazsa tarayıcıda `http://192.168.4.1` adresine gidin (bina ağı zaten `192.168.4.x` bloğundaysa `http://10.195.4.1` açılır).
   - Telefon sayfa açmıyorsa mobil veriyi kapatın ve "Bu ağda internet yok" uyarısında **Bağlı kal**'ı seçin.
4. Kurulum sayfasında listelenen ağlardan bina AP adını seçin (gizli ağ için **Diğer / gizli ağ** kullanın), bina Wi-Fi parolasını girin ve kaydedin.
   - **Beklenen Sonuç:** Bağlantı başarılı olduğunda panel LED'i **2 saniye sabit** yanar, `Doorbell-Setup` ağı kapanır ve panel beklemede **sönük** duruma geçer.
   - **Hata Durumu:** Parola hatalıysa veya AP'ye ulaşılamazsa panel LED'i **1 saniye hızlı yanıp söner** ve eski ayarlar korunur. (`Doorbell-Setup` ağına yeniden bağlanıp tekrar deneyin; kanal değişimi nedeniyle telefonda sonuç sayfası düşerse panel LED'ine bakın.)
5. **Fabrika Şifrelerini Değiştirin:** Panel ağa bağlandıktan (`LED sönük`) sonra etiket üzerindeki fabrika yönetici ve kullanıcı şifrelerini değiştirin (bkz. Bölüm 3.2).

### 1.3. Daire Modülü Eşleştirme (Panel Yanında)

Eşleştirme işlemi modüller dairelere monte edilmeden önce **panelin yanında** yapılır.

**Ön Koşullar:**

- Panelin bina Wi-Fi ağına bağlı ve hazır olduğundan emin olun (**panel LED'i sönük**).
- Daire modülüne enerji verin; eşleşmemiş modül LED'i **tekrarlayan çift flaş** yapar.
- **Tek Seferde Tek Modül:** Aynı anda yalnızca **bir** daire modülünün eşleştirme butonuna basın.

**Adımlar:**

1. **Panelde** ilgili daire numarası için eşleştirme penceresini açın:

   ```text
   * <yönetici> * 1 * <daire no> #
   ```

   - **Beklenen LED (Panel):** **Tekrarlayan çift flaş** (30 saniyelik keşif penceresi açılır; iptal için `* #`).
2. **Daire Modülünde** gizli **eşleştirme butonunu** basılı tutun:
   - **Tutma penceresi (7–12 s):** 7\. saniyede modül LED'i **orta hızda yanıp sönmeye** başladığında **butonu bırakın**.
   - **Zaman aşımı (> 12 s):** 12 saniyeden uzun basılırsa LED söner ve bırakmak işlemi tetiklemez; 15\. saniyede **üçlü flaş** ile işlem iptal edilir.
   - **Bırakma sonrası:** Modül LED'i **yavaş yanıp sönmeye (1 Hz)** geçer; modül geçici `Doorbell-Pair-XXXX` ağına bağlanır, bina Wi-Fi bilgisi ile kriptografik anahtarı alır ve ardından bina AP'sine bağlanarak eşleştirmeyi doğrular.
3. **Eşleştirme Sonucunu Doğrulayın:**

   | Durum | Panel LED | Modül LED / Zil |
   | :--- | :--- | :--- |
   | Başarılı | **2 s sabit yanar**, sonra sönük | **2 s tam parlak sabit yanar**, zil 1 kez çalar, sonra **loş sabit (%10)** |
   | Birden fazla modül algılandı | **1 s hızlı yanıp söner** (iptal) | Pencere bitince **tekrarlayan çift flaşa** döner |
   | Modül yok veya AP'ye bağlanamadı | **1 s hızlı yanıp söner** | **Tekrarlayan çift flaşa** döner |
   | Daire numarası dolu veya panel ağda değil | **1 s hızlı yanıp söner** | — |

4. Her modülün üzerine eşleştirildiği `<daire no>` bilgisini kalıcı olarak etiketleyin.

> **Not:** Eşleşmiş bir modülün eşleştirme butonuna basılırsa işlem **tekrarlayan üçlü flaş** ile reddedilir. Modülü başka daireye veya yeni panele almak için önce modülü sıfırlayın (Bölüm 3.5). Dolu daire numarasının üzerine yazılmaz; önce kaydı silin (Bölüm 3.4).

### 1.4. Daireye Montaj ve Teslim Testi

Eşleştirilen modülü ilgili daireye monte edip enerji verin:

1. **Kapsama Kontrolü:** Enerji geldikten birkaç saniye sonra modül LED'inin **loş sabit (%10)** yandığını doğrulayın. LED sürekli **yavaş yanıp sönüyorsa (1 Hz)** modül o noktada AP'ye bağlanamıyor veya panele ulaşamıyordur (AP kapsamasını/köprü ayarını kontrol edin).
2. **Kapı Açma Testi:** Daire modülündeki kapı açma butonuna basıp bırakın. Modül LED'i **2 s tam parlak sabit** yanmalı ve giriş kapısı kilidi **0,5 s** açılmalıdır.
3. **Zil Çalma Testi:** Panelden `<daire no> #` tuşlayın. Yalnızca doğru dairenin zili çalmalı, panel LED'i **2 s sabit** yanmalıdır.

---

## 2. Günlük Kullanım (Bina Sakini / Ziyaretçi)

### 2.1. Zil Çalma (Ziyaretçi — Panelden)

Daire numarasını başında sıfır olmadan tuşlayıp `#` tuşuna basın:

```text
12 #
```

| Panel LED Yanıtı | Anlamı |
| :--- | :--- |
| **2 saniye sabit yanar** | İlgili dairenin zili çaldı |
| **3 saniye orta hızda yanıp söner** | Daire kayıtlı ancak daire modülü cevap vermedi |
| **1 saniye hızlı yanıp söner** | Geçersiz numara veya bu numarada kayıtlı daire yok |

- Daire numaraları `1`–`99` arasındadır.
- Zil çalma, tuş takımı yanlış şifre kilidindeyken de çalışır.

### 2.2. Daireden Kapı Açma (Bina Sakini)

Daire modülündeki **kapı açma butonuna** basıp bırakın (komut buton bırakıldığında gönderilir).

| Modül LED Yanıtı | Anlamı |
| :--- | :--- |
| **2 saniye tam parlak sabit yanar** | Giriş kapısı kilidi 0,5 saniye açıldı |
| **3 saniye orta hızda yanıp söner** | Panel cevap vermedi; kapı açılmadı |
| **3 saniye orta hızda yanıp söner** (LED önce **tekrarlayan çift flaş**) | Modül eşleşmemiş; komut gönderilmedi (Bölüm 1.3) |

- 0,1 saniyeden kısa (parazit) veya 30 saniyeden uzun (sıkışmış buton) basışlar dikkate alınmaz.

### 2.3. Panelden Şifreyle Kapı Açma (Bina Sakini)

Panel tuş takımında `*`, ardından kullanıcı şifresi ve `#` tuşlayın:

```text
* <kullanıcı şifresi> #        (Örnek: * 1234 #)
```

- **Beklenen LED (Panel):** Şifre doğruysa kapı kilidi açılır ve LED **0,5 saniye sabit** yanar (varsayılan röle süresi). Şifre yanlışsa **1 saniye hızlı yanıp söner**.
- Kapı kilidi zaten açıkken (0,5 s içinde) gelen ikinci açma komutu yok sayılır.

### 2.4. Hatalı Girişi İptal Etme

Tuş takımında yazmakta olduğunuz komutu veya numarayı temizlemek için:

```text
* #
```

- 10 saniye boyunca hiçbir tuşa basılmazsa yazılan giriş kendiliğinden silinir.

---

## 3. Yönetim, Bakım ve Sıfırlama (Yönetici / Teknisyen)

### 3.1. Yönetici Komut Özeti

Komutlar, panelde eşleştirme penceresi veya kurulum portalı açık değilken girilir. Daire eşleştirme (`* 1`) için ayrıca panelin bina ağına bağlı olması (**LED sönük**) gerekir:

| İşlem | Komut | Başarı LED'i |
| :--- | :--- | :--- |
| Daire eşleştirme (30 s pencere) | `* <yönetici> * 1 * <daire no> #` | Tekrarlayan çift flaş → **2 s sabit** |
| Daire kaydını silme | `* <yönetici> * 2 * <daire no> * <daire no> #` | **2 s sabit** |
| Kullanıcı şifresi değiştirme | `* <yönetici> * 3 * <yeni 4 hane> * <yeni 4 hane> #` | **2 s sabit** |
| Yönetici şifresi değiştirme | `* <yönetici> * 4 * <yeni 8 hane> * <yeni 8 hane> #` | **2 s sabit** |
| Wi-Fi kurulum portalını açma | `* <yönetici> * 5 #` | **2 s sabit** → **1 Hz yavaş** (2 dk) |
| Bina sıfırlama (tüm daireleri sil) | `* <yönetici> * 6 * <yönetici> #` | **2 s sabit** ve yeniden başlar |

### 3.2. Şifre Değiştirme ve Yanlış Şifre Kilidi

- **Kullanıcı Şifresi Değiştirme (varsayılan 4 hane):**

  ```text
  * <yönetici> * 3 * <yeni kullanıcı şifresi> * <yeni kullanıcı şifresi> #
  ```

  - Kullanıcı şifresi yönetici şifresiyle aynı olamaz.
- **Yönetici Şifresi Değiştirme (varsayılan 8 hane):**

  ```text
  * <yönetici> * 4 * <yeni yönetici şifresi> * <yeni yönetici şifresi> #
  ```

  - Yönetici şifresi değiştiğinde `Doorbell-Setup` Wi-Fi portalının WPA3 parolası da otomatik olarak yeni yönetici şifresi olur.

**Yanlış Şifre Kilidi Politikası:**

- Üst üste **5 yanlış şifre** denemesinden sonra `*` ile başlayan tüm komutlar **60 saniye** kilitlenir (LED **seyrek tek flaş** yapar).
- Hatalı denemeler devam ederse kilit süresi katlanır (**120 s, 240 s … en fazla 15 dakika**). Kilit seviyesi flash hafızada tutulur; elektriği kesmek kilidi kaldırmaz.
- Doğru kullanıcı veya yönetici şifresi girildiğinde kilit seviyesi sıfırlanır.
- Söz dizimi hatalı girişler (örn. `**#` veya `* 1234 * #`) hata LED'i verir ancak yanlış şifre sayılmaz.

### 3.3. Bina Wi-Fi Ağını (SSID / Şifre) Değiştirme

Bina AP bilgileri değiştirilecekse:

1. Panelde `* <yönetici> * 5 #` ile `Doorbell-Setup` portalını açın ve Bölüm 1.2 Adım 3–4'teki gibi yeni SSID ve parolayı kaydedin.
   - Panel yeni AP'ye bağlanamazsa eski Wi-Fi ayarlarına otomatik geri döner (rollback) ve web arayüzünde hata gösterir.
2. **Modül Etkisi:** Güvenlik ve ağ yalıtımı gereği panelden daire modüllerine uzaktan Wi-Fi şifresi itilmez:
   - Yeni AP'ye **eski SSID ve parola** verilirse mevcut modüller sıfırlama gerektirmeden otomatik bağlanır.
   - Bina SSID veya Wi-Fi parolası kalıcı olarak değiştirildiyse **tüm daire modüllerinin sıfırlanıp (Bölüm 3.5) yeniden eşleştirilmesi (Bölüm 1.3)** gerekir.
3. **Seri Konsol Alternatifi (UART `115200` baud):** Panelin seri konsolundan kimlik doğrulamalı `doorbell_wifi <yönetici> <ssid> [parola]` komutuyla da bina Wi-Fi bilgileri güncellenebilir.

### 3.4. Daire Kaydı Silme

Panel hafızasından tek bir daireyi ve o dairenin kriptografik anahtarını kaldırmak için:

```text
* <yönetici> * 2 * <daire no> * <daire no> #
```

- **Beklenen LED (Panel):** **2 saniye sabit yanar**.
- Silinen daireye ait fiziksel modül artık panelden yanıt alamadığı için en geç bir sonraki durum raporunda (varsayılan yaklaşık 5 dakika) **yavaş yanıp sönmeye (1 Hz)** geçer. Modül tekrar kullanılacaksa sıfırlanıp (Bölüm 3.5) yeniden eşleştirilmelidir.

### 3.5. Daire Modülü Sıfırlama

Tek bir daire modülünün eşleştirme kaydını ve Wi-Fi bilgilerini silmek için:

1. Modül kutusundaki gizli **sıfırlama butonunu** basılı tutun.
2. **Tutma penceresi (7–12 s):** 7\. saniyede modül LED'i **sabit yanmaya başladığında (arada kısa söner)** butonu **bırakın**. (15\. saniyede **üçlü flaş** ile işlem iptal olur.)
3. Modül yeniden başlar ve LED **tekrarlayan çift flaş** (eşleşmemiş) durumuna geçer.

- **Panel Kaydı Kuralı:** Modülü sıfırlamak paneldeki eski kaydı silmez. Modülü **aynı** daire numarasıyla eşleştirmeden önce panelden eski kaydı silin (Bölüm 3.4). **Farklı** bir daire numarasıyla eşleştirirseniz panel eski kaydı yeni eşleştirme tamamlandığında otomatik siler.

### 3.6. Bina Sıfırlama (Tüm Daireleri Silme)

Şifreleri ve bina Wi-Fi ayarlarını **koruyarak** yalnızca tüm daire kayıtlarını ve anahtarlarını silmek için:

```text
* <yönetici> * 6 * <yönetici> #
```

- **Beklenen LED (Panel):** **2 saniye sabit yanar** ve panel yeniden başlar.
- Yeniden eşleştirme için bütün daire modülleri sıfırlanmalı (Bölüm 3.5) ve tek tek yeniden eşleştirilmelidir (Bölüm 1.3).

### 3.7. Panel Fabrika Sıfırlama, Panel Değişimi ve Yeniden Başlatma

#### Panel Fabrika Sıfırlama (Gizli Buton)

Yönetici/kullanıcı şifreleri unutulduğunda veya paneli ilk yükleme (flaşlama) durumuna döndürmek gerektiğinde:

1. Panel kutusu içindeki gizli **fabrika sıfırlama butonunu** basılı tutun.
2. **Tutma penceresi (10–60 s):** 10\. saniyede LED **sabit (arada kısa sönme)** düzenine geçtiğinde butonu **bırakın** (60 saniyeyi aşan basışlar sıkışmış buton sayılır ve yok sayılır).
3. Panel **2 s sabit** yanıtından sonra yeniden başlar:
   - Yönetici ve kullanıcı şifreleri ile `Doorbell-Setup` parolası **cihaz etiketindeki fabrika değerlerine** döner.
   - Bina Wi-Fi bilgisi flaşlama anında tanımlanan değere döner (boş yüklenmişse boş kalır; Bölüm 1.2 ile ayarlayın).
   - Yanlış şifre kilidi kalkar.
   - **Bütün daire kayıtları silinir;** tüm modüller sıfırlanıp yeniden eşleştirilmelidir.
   - Fabrika anlık görüntüsü (provisioning kaydı) eksikse sıfırlama reddedilir ve LED **1 s hızlı yanıp söner**.

#### Panel Değişimi

Daire kayıtları ve anahtarlar güvenlik gereği yalnızca panel NVS belleğinde tutulur; yedek alınamaz veya başka panele aktarılamaz. Panel değiştirildiğinde:

1. Yeni panelin Flasher ile cihaza özel şifreler yüklenerek hazırlandığından ve etiketinin takıldığından emin olun. (Flasher ile hazırlanmamış panel şifre kabul etmez ve her 10 saniyede bir **1 s hızlı** yanıp söner.)
2. Yeni panelde bina Wi-Fi ağını kurun (Bölüm 1.2) ve fabrika şifrelerini değiştirin (Bölüm 3.2).
3. Tüm daire modüllerini sıfırlayın (Bölüm 3.5).
4. Modülleri yeni panelle yeniden eşleştirin (Bölüm 1.3).

#### Yeniden Başlatma (Veri Kaybı Olmaz)

- **Elle Yeniden Başlatma:** Kart üzerindeki **EN** butonuna basın veya enerjiyi kesip verin (tüm şifreler, Wi-Fi ayarları, kilit sayacı ve daire kayıtları korunur).
- **Otomatik Kurtarma:** Görev kilitlenmesinde watchdog yeniden başlatma tetikler. Ayrıca en az bir daire kayıtlıyken hiçbir modülden **3 heartbeat aralığı** (varsayılan 15 dakika) boyunca rapor alınamazsa panel kendini yeniden başlatır; sessizlik devam ederse yeniden başlatma aralığı üstel artarak en fazla **48 saate** (30 dk, 1 sa, 2 sa …) kadar uzar ve ilk doğrulanan modül paketinde 15 dakikaya sıfırlanır.

---

## 4. LED Göstergeleri ve Arıza Teşhisi (Hızlı Referans)

Birden fazla durum aynı anda oluşursa aşağıdaki tablolarda **üst sırada yer alan (yüksek öncelikli)** gösterge görünür; geçici uyarı bittiğinde LED önceki kalıcı durumuna döner.

### 4.1. Panel LED Öncelik Tablosu

| Öncelik | LED Deseni | Anlamı |
| :---: | :--- | :--- |
| 1 | **Tek kısa flaş (50 ms)** | Tuşa basıldı |
| 2 | **Sabit yanar (varsayılan 0,5 s)** | Kapı açma rölesi çekili |
| 3 | **2 saniye sabit yanar** | İşlem başarılı / zil çaldı / eşleştirme tamam / portal açıldı |
| 4 | **1 saniye hızlı yanıp söner** | Komut hatası / yanlış şifre / daire dolu / çoklu modül (sürekli tekrarda: depolama hatası) |
| 5 | **3 saniye orta hızda yanıp söner** | Daire kayıtlı ancak modül cevap vermedi |
| 6 | **Sabit, arada kısa sönme (900/100 ms)** | Gizli fabrika sıfırlama butonu geçerli aralıkta (10–60 s): şimdi bırakın |
| 7 | **Yavaş yanıp sönme (1 Hz)** | Tuş takımında komut/numara yazılıyor |
| 8 | **Tekrarlayan çift flaş** | 30 saniyelik daire eşleştirme penceresi açık |
| 9 | **Yavaş yanıp sönme (1 Hz)** | `Doorbell-Setup` Wi-Fi kurulum portalı açık (2 dakika) |
| 10 | **Seyrek tek flaş (3 s arayla)** | Yanlış şifre kilidi aktif (`*` komutları kilitli) |
| 11 | **Seyrek tek flaş (3 s arayla)** | Panel bina Wi-Fi ağına bağlı değil |
| 12 | **Sönük** | Normal bekleme: ağa bağlı ve hazır |

### 4.2. Daire Modülü LED Öncelik Tablosu

| Öncelik | LED Deseni | Anlamı |
| :---: | :--- | :--- |
| 1 | **Hızlı yanıp sönme** | Zil çalıyor (görsel zil, röle süresince) |
| 2 | **Tekrarlayan üçlü flaş (~2 s)** | Buton ≥ 15 s basıldı veya geçersiz basım; iptal (sürekli tekrarda: depolama hatası) |
| 3 | **Orta hızda yanıp sönme** | Gizli eşleştirme butonu geçerli aralıkta (7–12 s): şimdi bırakın |
| 4 | **Sabit, arada kısa sönme (900/100 ms)** | Gizli sıfırlama butonu geçerli aralıkta (7–12 s): şimdi bırakın |
| 5 | **2 saniye tam parlak sabit** | Kapı açıldı / eşleştirme başarılı |
| 6 | **3 saniye orta hızda yanıp söner** | Kapı açılamadı (panel cevap vermiyor) |
| 7 | **Yavaş yanıp sönme (1 Hz)** | Eşleştirme sürüyor (panel ağına ve bina ağına bağlanılıyor) |
| 8 | **Tekrarlayan çift flaş** | Modül eşleşmemiş |
| 9 | **Yavaş yanıp sönme (1 Hz)** | Hata / kapsama dışı: ağa bağlanamıyor veya ağda panele ulaşılamıyor (kayıt silinmiş / panel kapalı) |
| 10 | **Loş sabit (%10)** | Normal çalışma: eşleşmiş, ağa bağlı ve panel erişilebilir |

### 4.3. Saha Arıza Teşhis Tablosu

Panelin seri monitörü (`UART0`, `115200` baud) üzerinden tüm modüllerin durum raporları (`rssi`, `ch`, `disconn`, çalışma süresi, son reset nedeni) ve `doorbell_status` çıktısı izlenebilir.

| Belirti | Olası Neden | Çözüm |
| :--- | :--- | :--- |
| Zil çalınca panel **3 s orta hızda** yanıp sönüyor | Modülün enerjisi yok, AP/tekrarlayıcı kapalı veya modül kapsama dışında | Modül beslemesini, AP'yi ve modül LED'ini kontrol edin; AP'de istemci izolasyonunun kapalı olduğundan emin olun |
| Zil çalınca panel **1 s hızlı** yanıp sönüyor | Numara geçersiz veya o daire kayıtlı değil | Daire numarasını (1–99) doğrulayın; gerekiyorsa modülü eşleştirin |
| Eşleştirme başlamıyor veya hata veriyor | Panel ağa bağlı değil, daire dolu, modül zaten eşleşmiş veya birden fazla modül eşleştirme modunda | Panel LED'inin sönük olduğunu doğrulayın; eski daire kaydını silin (3.4), modülü sıfırlayın (3.5) ve aynı anda tek modül deneyin |
| Eşleşmiş modül sürekli **1 Hz yavaş** yanıp sönüyor | AP kapalı, Wi-Fi sinyali yetersiz, panel kapalı, bina SSID/şifresi değişmiş ya da kayıt panelden silinmiş | AP ve paneli kontrol edin; AP'de istemci izolasyonunun kapalı olduğunu doğrulayın; kayıt silindiyse modülü sıfırlayıp yeniden eşleştirin |
| Kapı açma butonunda modül **3 s orta hızda** yanıp sönüyor | Panel kapalı veya AP bağlantısı kesilmiş | Panel beslemesini ve AP ağ bağlantısını kontrol edin |
| `*` komutları anında **1 s hızlı** hata veriyor, panel **seyrek tek flaş** yapıyor | Yanlış şifre kilidi | Kilit süresinin bitmesini bekleyin (en fazla 15 dk) |
| Panel **seyrek tek flaş** yapıyor ama şifreler çalışıyor | Panel bina Wi-Fi ağına bağlı değil | AP'yi kontrol edin; SSID veya parola değiştiyse portalı açıp (Bölüm 1.2) yeniden kaydedin |
| Panel her 10 saniyede bir **1 s hızlı** yanıp sönüyor, hiçbir şifre çalışmıyor | Panel yapılandırma kaydı eksik veya bozuk | Paneli Flasher ile yeniden yükleyin. **Tüm daire kayıtları silinir;** modülleri yeniden eşleştirin |
| Tüm şifreler unutuldu | — | Panel fabrika sıfırlama yapın (3.7); etiket şifrelerine döner ve tüm modüller yeniden eşleştirilir |
