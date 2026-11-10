# Yapılandırma Ayarları

Flaşlama programının (GUI) **Configuration** (`Kconfig`) bölümündeki ayarların ne işe yaradığı. Değerler firmware'e
derleme sırasında gömülür. Bir ayarı değiştirdikten sonra firmware'i yeniden derleyip cihazlara yükleyin (paneldeki bina
Wi-Fi adı ve şifresi NVS'de tutulduğu için yeniden derlemeden web portalı veya seri konsoldan da değiştirilebilir).

- **Varsayılan** sütunu, hiçbir şey değiştirilmediğinde kullanılan değerdir.
- Bazı varsayılanlar henüz tezgahta veya sahada doğrulanmadı; bunlar "ölçülecek" diye işaretlidir.
- **Doorbell network** menüsündeki ortak ayarlar (`UDP port`, `Heartbeat interval` vb.) panelde ve tüm daire
  modüllerinde **aynı** olmalıdır (panel ayrıca eşleştirme sırasında `Heartbeat interval` değerini modüle aktarır).
- İmkânsız bir kombinasyon (örneğin en kısa süre en uzundan büyük veya yönetici şifresi 8 haneden kısa) derleme
  sırasında (`_Static_assert` ile) hata verir.

---

## 1. Doorbell network (panel ve modül)

### 1.1. UDP ağı ve regülasyon

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| UDP port | 19500 | Panel ve daire modüllerinin UDP haberleşmesinde kullandığı port (IANA listesinde atanmamış User Port aralığındadır ve lwIP ephemeral port aralığı olan 49152–65535 ile çakışmaz). |
| Set a Wi-Fi country code | Kapalı | Açılırsa aşağıdaki ülke kodu uygulanır. Kapalıyken ESP-IDF'in varsayılan ülke ayarı (`01` — kanal 1–11 aktif, kanal 12–13 pasif tarama) kullanılır. |
| Country code | (boş) | İki harfli ülke kodu, örn. `TR` (kanal 1–13, maks. 20 dBm). Bina AP'si kanal 12 veya 13'te çalışıyorsa açılmalıdır. |
| Restrict the station interface to 802.11b | Açık | Cihaz bina AP'sine yalnızca 802.11b hızlarıyla (1–11 Mbps) bağlanır. Düşük hızlar daha zayıf sinyalde çalıştığı için menzili artırabilir. Bina AP'sinde "802.11b'yi kapat" veya 11 Mbps üzeri asgari hız ayarı varsa cihaz bağlanamaz; bu durumda AP ayarı değiştirilmeli ya da bu seçenek kapatılmalıdır. Kapatılırsa ESP-IDF varsayılanı (802.11b/g/n) kullanılır. |

- **Bina erişim noktası (AP) ve ağ kurulum kontrol listesi:**
  1. **Görünür SSID ve Sabit Kanal:** 2,4 GHz bandında birbiriyle çakışmayan **1, 6 veya 11.** kanallardan biri sabit
     seçilmeli ve SSID görünür olmalıdır.
  2. **Çoklu AP / Güçlendirici:** Yüksek binalarda en güvenilir yöntem aynı SSID ve WPA2/WPA3 şifresini yayınlayan
     **kablolu erişim noktalarıdır**. Kablosuz güçlendirici (repeater) kullanılıyorsa aynı SSID/şifreyi korumalı,
     **köprü (Layer-2 bridge) modunda** olmalı (NAT veya `_EXT` uzantısı olmamalı) ve en fazla 1 kablosuz aktarma
     (hop) içermelidir.
  3. **İzolasyon ve Filtreler Kapalı:** Erişim noktasında **istemci izolasyonu (AP/client isolation)**, broadcast/multicast
     filtreleme ve proxy-ARP **kapalı** olmalıdır.
  4. **DHCP Kapasitesi:** DHCP sunucusu açık olmalı ve IP havuzu tüm daire modüllerini + paneli kapsayacak büyüklükte
     olmalıdır (240'tan fazla cihaz varsa `/23` alt ağı kullanılmalıdır). Panel için DHCP'de sabit IP ayrılması
     (DHCP reservation) önerilir. Cihazlar ağda `doorbell-panel` ve `doorbell-apt-012` biçiminde görünür.
  5. **802.11b Hızları Açık:** Cihazlar varsayılan olarak yalnızca 802.11b hızlarıyla (1–11 Mbps) bağlanır. Erişim
     noktasında "802.11b'yi devre dışı bırak", "legacy hızları kapat" veya 11 Mbps üzeri asgari temel hız (minimum
     basic rate) ayarı **kapalı** olmalıdır.
- **En güçlü erişim noktasını seçme:** Cihazlar tüm kanalları tarayıp (`WIFI_ALL_CHANNEL_SCAN`) sinyali en güçlü olan
  erişim noktasına (`WIFI_CONNECT_AP_BY_SIGNAL`) bağlanır; bağlantı koparsa üstel artan bekleme süresiyle (1–60 s)
  sürekli yeniden bağlanmayı dener.
- **IP adresleme ve çift yollu tekrar gönderim:** İlk mesaj denemesi hedef cihazın bilinen son IP adresine (unicast),
  cevap alınamazsa tekrar denemeler hem son bilinen IP adresine (unicast) hem de `255.255.255.255` sınırlı yayın
  (limited broadcast) adresine gönderilir. IP tablosu yalnızca kimlik doğrulaması (AES-128-CCM) geçmiş paketlerden
  güncellenir.
- **Wi-Fi güç tasarrufu:** Cihazlar şebekeden beslendiği ve 400 ms ACK süresinin gecikmemesi gerektiği için STA güç
  tasarrufu kapalıdır (`WIFI_PS_NONE`).

### 1.2. Uygulama mesajları

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Application ACK timeout | 400 ms | Zil, kapı açma veya durum raporu gönderildikten sonra "aldım" cevabının (ACK) her denemede ne kadar bekleneceği (±25 ms rastgele sapma ile). Masada ve sahada doğrulandı. Panel ve modülde **aynı** olmalıdır (Flasher farklıysa derlemeyi durdurur). |
| Pairing window | 20 s | Panelin eşleşmemiş modülü yakalama (keşif) penceresi süresi. |
| Pairing completion timeout | 10 s | Modül yakalandıktan sonra bina erişim noktasına (AP) bağlanıp DHCP'den IP alarak son onayı (`PAIR_COMPLETE`) göndermesi için tanınan ek tamamlama süresi. En az `5 s + 4 × ACK timeout + 0,5 s` olmalıdır (400 ms ile 7,1 s); daha kısaysa derleme hata verir. Panel ve modülde **aynı** olmalıdır. |
| Replay counter NVS block size | 100 | Mesaj sayacının flash'a kaç mesajda bir önceden резерв edilerek (write-ahead) yazıldığı. |
| Module health reports (heartbeat) | Açık | Her modül belirli aralıklarla panele onaylı durum raporu gönderir; panel her raporu loglar ve `ACK` döner. Kapatılırsa "no report" uyarıları, sessizlikte yeniden başlatma ve modülün "panele ulaşılamıyor" LED uyarısı devre dışı kalır. |
| Heartbeat interval | 300 s | İki rapor arasındaki süre (±%10 rastgele sapma ile; elektrik kesintisi dönüşünde ilk rapor `[5 s .. Heartbeat interval]` arasında rastgele gecikmeyle gönderilir). |

**ACK ve tekrar gönderim örneği:** 400 ms ve zil tekrarı 2 ise panel, "cevap vermiyor" göstermeden önce en fazla
3 × 400 ms = 1,2 saniye bekler. Alıcı cihaz son gönderdiği ACK paketini önbellekte tutar; ACK paketi yolda kaybolursa
tekrar gelen aynı pakete zili ikinci kez çaldırmadan veya kapı açıkken hata vermeden tekrar ACK döner. Monitördeki
`acknowledged after ... ms` satırları gerçek süreyi gösterir.

**Sayaç bloğu (write-ahead) örneği:** 100 ise cihaz bir bloktaki sayaçları kullanmadan **önce** üst sınırı (+100)
flash'a yazar. Yeniden başladığında sayaç en fazla 100 ileri atlar; böylece aynı AES-CCM tek kullanımlık sayısı (nonce)
asla iki kez kullanılmaz.

**Heartbeat (onaylı durum raporu):** Rapor; modülün çalışma süresini, son yeniden başlama nedenini (ESP32'de elektrik
kesintisi ve EN butonu `POWERON` olarak görünür), aldığı zil sayısını, cevapsız kalan kapı açma isteklerini, kendisine
gelip doğrulamadan geçemeyen (reddedilen) mesajları, Wi-Fi kopma sayısını, bağlı olduğu erişim noktasından (AP) aldığı
sinyal gücünü (RSSI), kanal/BSSID bilgisini, yazılım sürümünü ve boş belleği (anlık ve en düşük) içerir. Başka dairelere
giden broadcast paketler başlıkta elenir ve reddedilen mesaj sayacını artırmaz. Bir modülden 3 aralık boyunca rapor
gelmezse panel "no report" uyarısı yazar; modül de panelden 3 aralık boyunca `HEARTBEAT_ACK` alamazsa LED'ini seyrek tek
flaşa geçirir.

- **Aralık örneği:** UDP yapısında trafik modül sayısıyla doğrusal artar. 99 modüllü bir binada 300 s (5 dakika) aralık
  saniyede ortalama yalnızca 0,33 rapor üretir.

---

## 2. Doorbell panel (yalnızca panel)

### 2.1. Bina Wi-Fi ve kurulum portalı (yalnızca panel)

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Default Wi-Fi SSID | `(boş)` | Bina erişim noktasının (AP) başlangıç Wi-Fi adı. Yalnızca ilk açılışta NVS boşsa kullanılır. |
| Default Wi-Fi password | `(boş)` | Bina erişim noktasının başlangıç WPA2 şifresi (kaynak kod deposuna gerçek şifre yazılmaması için varsayılanı boştur). Sahada `* <yönetici> * 5 #` portalı veya UART konsolu ile NVS'ye kaydedilir. |
| Wi-Fi setup portal timeout | 120 s | Panelde `* <yönetici> * 5 #` komutu girildiğinde açılan `Doorbell-Setup` kurulum ağının (`http://192.168.4.1`) açık kalma süresi (2 dakika). |

### 2.2. GPIO (Klasik ESP32)

Pin numaraları klasik **ESP32** (`IDF_TARGET_ESP32`) içindir; kablolama varsayılandan farklıysa değiştirilir. Klasik
ESP32'de GPIO 6–11 flash'a ayrılmıştır; GPIO 34–39 sadece giriştir ve dahili pull-up'ı yoktur (`ESP32-C3` veya
`ESP32-S3` hedeflerinde o çipe uygun geçerli GPIO pinleri seçilmelidir).

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Keypad row 0–3 | 13, 14, 27, 26 | Tuş takımı satır pinleri |
| Keypad column 0–2 | 25, 33, 32 | Tuş takımı sütun pinleri |
| Door relay | 23 | Kapı kilidi rölesi (açılışta pin yönü ayarlanmadan önce inaktif seviye yazılır) |
| Door relay is driven LOW to open | Açık | Röle modülü LOW ile çekiyorsa açık kalmalı. **Yanlış ayarlanırsa kilit beklemede açık durur.** |
| Status LED | 2 | Durum LED'i |
| Hidden code reset button | 19 | Gizli şifre sıfırlama butonu |
| Code reset button pulls the GPIO LOW | Açık | Buton GND'ye çekiyorsa açık |
| Enable the internal pull resistor | Açık | Dahili direnci kullanır; harici direnç varsa kapatılabilir |

### 2.3. Keypad

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Key map | `123456789*0#` | 12 karakter: satırlar yukarıdan aşağı, sütunlar soldan sağa. Tuş takımı farklı dizilmişse buradan düzeltilir. |
| Input timeout | 10 s | Bu süre tuşa basılmazsa yazılan silinir. |

### 2.4. Door relay

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Door relay pulse | 500 ms | Kapı kilidinin açık tutulduğu ve panel LED'inin sabit yandığı süre. Kilit tipine göre ayarlanır. |

### 2.5. Apartments and codes

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Lowest / Highest apartment number | 1 / 99 | Geçerli daire numarası aralığı (1–99; varsayılan 24 KB NVS bütçesine uygun üst sınır). |
| Admin code length | 8 | Yönetici şifresinin hane sayısı (8–10). Wi-Fi ayar portalının (`Doorbell-Setup`) WPA3-SAE şifresi olarak da kullanıldığı için en az 8 hane olmalıdır. |
| User code length | 4 | Kullanıcı şifresinin hane sayısı (4–8). |

Şifre uzunluğu değişirse fabrika şifreleri de aynı uzunlukta yüklenmelidir. Flaşlama programı şifreleri formdaki
uzunluğa göre kontrol eder; eski uzunlukla derlenmiş bir firmware yüklenirse panel şifreleri kabul etmez (ADR 0014).

### 2.6. Keypad lockout

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Wrong codes before lockout | 5 | Kaç yanlış şifreden sonra kilitleneceği |
| First lockout | 60 s | İlk kilit süresi |
| Lockout multiplier | 2 | Her yeni kilitte sürenin kaç katına çıkacağı. **1** sabit süre demektir. |
| Maximum lockout | 900 s | Kilit süresinin üst sınırı (15 dakika) |

**Örnek:** 3 deneme, 30 saniye, çarpan 3 ve en fazla 600 saniye ise kilit süreleri 30, 90, 270 ve 600 saniye olur;
sonrasında hep 600 saniyede kalır (kilit durumu NVS'de saklanır, elektrik kesip açmak kilidi sıfırlamaz). Doğru şifre
seviyeyi sıfırlar.

### 2.7. Code reset button

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Minimum hold | 10 s | Bundan kısa basış şifreleri sıfırlamaz |
| Maximum hold | 60 s | Bundan uzun basış (sıkışmış buton) sıfırlamaz |

### 2.8. Diğer

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| RING retries without ACK | 2 | Cevap gelmezse zilin kaç kez daha gönderileceği. 2 ise toplam 3 gönderim yapılır. |
| Restart the panel when no module reports | Açık | En az bir daire kayıtlıyken hiçbir modülden 3 heartbeat aralığı boyunca rapor gelmezse panel kendini yeniden başlatır. Heartbeat kapalıysa bu ayar görünmez. |

**Sessizlikte yeniden başlatma örneği:** Heartbeat 300 s ise hiçbir modülden 15 dakika rapor gelmezse panel yeniden
başlar. Sonra yine rapor gelmezse bekleme süresi (`RTC_NOINIT` bellekte korunarak) her seferinde iki katına çıkar: 30
dakika, 1 saat, 2 saat … ve en fazla 48 saat. İlk doğrulanmış rapor geldiğinde sayaç sıfırlanır.

---

## 3. Doorbell apartment module (yalnızca daire modülü)

### 3.1. GPIO (Klasik ESP32)

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Bell relay | 4 | Zil rölesi (açılışta pin yönü ayarlanmadan önce inaktif seviye yazılır) |
| Bell relay is driven HIGH to ring | Açık | Röle HIGH ile çekiyorsa açık |
| Door-open button | 23 | Kapı açma butonu |
| Hidden pairing button | 18 | Gizli eşleştirme butonu |
| Hidden reset button | 19 | Gizli sıfırlama butonu |
| Buttons pull the GPIO LOW when pressed | Açık | Butonlar GND'ye çekiyorsa açık |
| Enable the internal pull resistors | Açık | Dahili dirençleri kullanır |
| Status LED | 2 | Durum LED'i |

### 3.2. Bell

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Bell relay pulse | 500 ms | Zil rölesinin çekili kaldığı süre |
| Blink the status LED while ringing | Açık | Görsel zil: zil çalarken LED hızlı yanıp söner (işitme engelliler için, en yüksek LED önceliğine sahiptir) |

### 3.3. Buttons

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| Door button minimum press | 100 ms | Daha kısa basışlar parazit sayılır |
| Door button maximum press | 30 s | Daha uzun basışlar sıkışmış buton sayılır |
| Pairing/reset button minimum hold | 7 s | Eşleştirme ve sıfırlama için en kısa basılı tutma |
| Pairing/reset button maximum hold | 12 s | En uzun basılı tutma; bundan sonra bırakmak işe yaramaz |
| Pairing/reset button cancel after | 15 s | Bu sürede LED üçlü flaş yapar (iptal). En fazla 65 s; en uzun basılı tutmadan büyük olmalı. |

### 3.4. Diğer

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| OPEN_DOOR retries without ACK | 1 | Cevap gelmezse kapı açma isteğinin kaç kez daha gönderileceği. 1 ise toplam 2 gönderim yapılır. |

---

## 4. Doorbell status LED (panel ve modül)

| Ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| LED is on when the GPIO is HIGH | Açık | LED kutbu. LED ters çalışıyorsa (beklemede yanık) kapatın. Kart şemasına göre doğrulanmadı. |
| Full brightness | %100 | Tam parlaklık (`led_indicator` gama düzeltmeli maksimum seviye: 255) |
| Dim brightness | %10 | Modülün "loş, sabit" (normal) durumundaki algılanan parlaklık (gama düzeltmesi sonrasında tezgahta görünürlüğü doğrulanacak) |

**Pattern timing:** LED desenlerinin süreleri. Kısa flaş 50 ms, hızlı yanıp sönme periyodu 200 ms, orta 500 ms,
yavaş 1000 ms (1 Hz), çift/üçlü flaş darbesi 100 ms, arası 100 ms, tekrar periyodu 1000 ms, seyrek flaş aralığı
3000 ms, sıfırlama butonu basılı tutma darbesi 900 ms açık / 100 ms kapalı. Başarı 2000 ms, hata 1000 ms, cevap yok
3000 ms gösterilir. Her adım en az bir sistem tick'i (10 ms) olmalıdır; olmazsa derleme (`_Static_assert` ile) hata
verir.

---

## 5. Espressif ve ESP-IDF ayarları

Formun altındaki **IoT Button**, **LED Indicator** ve seçilmiş ESP-IDF (`v6.1`) ayarları Espressif'in kendi
bileşenlerine aittir. Bir sebep olmadıkça varsayılanda bırakın.

| Menü / ayar | Varsayılan | Ne işe yarar |
| :--- | :--- | :--- |
| IoT Button: period time / debounce ticks | 5 ms / 2 tick | Butonların ve tuş takımının tarama aralığı (5 ms) ve parazit filtresi (2 tarama = 10 ms). Butonlar çift algılıyorsa debounce artırılır. |
| LED Indicator | LEDC PWM default | LED'in PWM frekansı, çözünürlüğü ve gama düzeltmesi |
| CPU frequency | 160 MHz | İşlemci hızı |
| Brownout detector | Açık | Besleme gerilimi düşünce cihazı güvenli şekilde yeniden başlatır. **Açık kalmalı.** |
| Interrupt watchdog | Açık (300 ms) | Kesmeleri uzun süre kapatan bir hatada cihazı yeniden başlatır (PSRAM olmayan kartlarda 300 ms). **Açık kalmalı.** |
| Task watchdog / panic / timeout | Panic açık, 5 s | Kilitlenen veya CPU'yu bırakmayan bir görevde cihazı yeniden başlatır. Uygulama görevleri ve zamanlayıcı hatları watchdog'a kaydedilir. Panic kapatılırsa watchdog sadece uyarı yazar. |
| Console UART / baud rate | UART0 / 115200 | Seri monitör portu ve hızı |
| Flash mode / frequency / size | DIO / 40 MHz / 2 MB | Flash bellek ayarları. Boyut 2 MB'tır; kartla uyumlu olmalı (OTA kullanılmaz, ADR 0013). |
| Compiler optimization | Development: Debug (`-Og`) · Production: Size (`-Os`) | Flasher'daki profil seçer. Production (üretim) derlemesi uygulamayı küçültür ve 1 MB'lık uygulama bölümünde yer açar; Development hata ayıklamaya uygun mevcut ayardır. Assert kontrolleri ikisinde de açıktır. Production profili eFuse yakmaz. |
| Burn profili (kilitli cihaz) | Kapalı (yalnızca Burn seçilince) | Production derlemesine ek olarak Secure Boot v2, flash şifreleme (release), NVS şifreleme ve UART indirme modunun kalıcı kapatılmasını açar (`firmware/sdkconfig.burn`, `firmware/partitions_burn.csv`, ADR 0015). Kart ilk açılışta eFuse yakar: bir daha firmware yüklenemez, okunamaz, silinemez. Şifreler, Wi-Fi ve eşleştirme çalışma sırasında değiştirilebilir kalır; derleme ayarları (bu tablodaki tüm satırlar) değiştirilemez. Saklanacak bir anahtar yoktur: her Burn derlemesi kendi tek kullanımlık imza anahtarını üretir, firmware'i imzalar ve anahtarı siler. |
| Default log verbosity | Info | Monitörde ne kadar ayrıntı görüneceği (şifreler ve anahtarlar hiçbir seviyede loglanmaz). |
