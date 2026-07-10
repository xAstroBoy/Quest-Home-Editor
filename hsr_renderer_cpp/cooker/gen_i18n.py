#!/usr/bin/env python3
# Generates src/ui/i18n.h — the editor's UI translation table (GitHub #10).
# English is the default/key; each other language is a column. Missing cells fall back to English.
# UTF-8 is emitted as \xNN escapes so the header is pure-ASCII and compiler-safe.
#
# Add a language: append (code, native-name, script-tag) to LANGS and fill its column in T.
#   script tags: latin, cyrillic, cjk, hangul, kana, arabic  (drives font-fallback glyph baking)
import os

# (enum id, native menu label, primary script) — EN first (default).
LANGS = [
    ("EN", "English",      "latin"),
    ("ZH", "中文",             "cjk"),      # Chinese (Simplified)
    ("ES", "Español",            "latin"),
    ("FR", "Français",           "latin"),
    ("DE", "Deutsch",            "latin"),
    ("IT", "Italiano",           "latin"),
    ("PT", "Português",           "latin"),
    ("NL", "Nederlands",         "latin"),
    ("PL", "Polski",             "latin"),
    ("TR", "Türkçe",              "latin"),
    ("VI", "Tiếng Việt",         "latin"),
    ("RU", "Русский",             "cyrillic"),
    ("UK", "Українська",         "cyrillic"),
    ("JA", "日本語",           "kana"),     # Japanese
    ("KO", "한국어",            "hangul"),   # Korean
    ("AR", "العربية",           "arabic"),   # Arabic (unshaped, isolated forms)
]

# T[english] = { "ZH": "...", "ES": "...", ... }.  Any language omitted for a key falls back to English.
T = {
 # ── top bar / menus / global actions ──
 "File":        {"ZH":"文件","ES":"Archivo","FR":"Fichier","DE":"Datei","IT":"File","PT":"Arquivo","NL":"Bestand","PL":"Plik","TR":"Dosya","VI":"Tệp","RU":"Файл","UK":"Файл","JA":"ファイル","KO":"파일","AR":"ملف"},
 "Edit":        {"ZH":"编辑","ES":"Editar","FR":"Édition","DE":"Bearbeiten","IT":"Modifica","PT":"Editar","NL":"Bewerken","PL":"Edycja","TR":"Düzen","VI":"Sửa","RU":"Правка","UK":"Редаг.","JA":"編集","KO":"편집","AR":"تحرير"},
 "Object":      {"ZH":"对象","ES":"Objeto","FR":"Objet","DE":"Objekt","IT":"Oggetto","PT":"Objeto","NL":"Object","PL":"Obiekt","TR":"Nesne","VI":"Đối tượng","RU":"Объект","UK":"Об'єкт","JA":"オブジェクト","KO":"객체","AR":"كائن"},
 "View":        {"ZH":"视图","ES":"Vista","FR":"Vue","DE":"Ansicht","IT":"Vista","PT":"Ver","NL":"Beeld","PL":"Widok","TR":"Görünüm","VI":"Xem","RU":"Вид","UK":"Вигляд","JA":"表示","KO":"보기","AR":"عرض"},
 "Save":        {"ZH":"保存","ES":"Guardar","FR":"Enregistrer","DE":"Speichern","IT":"Salva","PT":"Salvar","NL":"Opslaan","PL":"Zapisz","TR":"Kaydet","VI":"Lưu","RU":"Сохранить","UK":"Зберегти","JA":"保存","KO":"저장","AR":"حفظ"},
 "Load":        {"ZH":"加载","ES":"Cargar","FR":"Charger","DE":"Laden","IT":"Carica","PT":"Carregar","NL":"Laden","PL":"Wczytaj","TR":"Yükle","VI":"Tải","RU":"Загрузить","UK":"Завант.","JA":"読込","KO":"불러오기","AR":"تحميل"},
 "Open":        {"ZH":"打开","ES":"Abrir","FR":"Ouvrir","DE":"Öffnen","IT":"Apri","PT":"Abrir","NL":"Openen","PL":"Otwórz","TR":"Aç","VI":"Mở","RU":"Открыть","UK":"Відкрити","JA":"開く","KO":"열기","AR":"فتح"},
 "Cook APK":    {"ZH":"烘焙 APK","ES":"Compilar APK","FR":"Compiler l'APK","DE":"APK erstellen","IT":"Compila APK","PT":"Gerar APK","NL":"APK bakken","PL":"Buduj APK","TR":"APK Pişir","VI":"Nướng APK","RU":"Собрать APK","UK":"Зібрати APK","JA":"APKをビルド","KO":"APK 구우기","AR":"إنشاء APK"},
 "Blender ->":  {"ZH":"导出 Blender","ES":"Exportar a Blender","FR":"Export Blender","DE":"Nach Blender","IT":"Esporta a Blender","PT":"Exportar p/ Blender","NL":"Naar Blender","PL":"Do Blender","TR":"Blender'a Aktar","VI":"Xuất Blender","RU":"В Blender","UK":"У Blender","JA":"Blenderへ","KO":"Blender로","AR":"إلى Blender"},
 "-> Blender":  {"ZH":"导入 Blender","ES":"Importar de Blender","FR":"Import Blender","DE":"Von Blender","IT":"Importa da Blender","PT":"Importar do Blender","NL":"Van Blender","PL":"Z Blender","TR":"Blender'dan Al","VI":"Nhập Blender","RU":"Из Blender","UK":"З Blender","JA":"Blenderから","KO":"Blender에서","AR":"من Blender"},
 "Auto-save: On":  {"ZH":"自动保存: 开","ES":"Autoguardado: Sí","FR":"Sauvegarde auto : Oui","DE":"Auto-Speichern: An","IT":"Salv. auto: Sì","PT":"Auto-salvar: Lig","NL":"Autom. opslaan: Aan","PL":"Autozapis: Wł.","TR":"Oto-kaydet: Açık","VI":"Tự lưu: Bật","RU":"Автосохр.: Вкл","UK":"Автозбер.: Увім","JA":"自動保存: オン","KO":"자동저장: 켜짐","AR":"حفظ تلقائي: تشغيل"},
 "Auto-save: Off": {"ZH":"自动保存: 关","ES":"Autoguardado: No","FR":"Sauvegarde auto : Non","DE":"Auto-Speichern: Aus","IT":"Salv. auto: No","PT":"Auto-salvar: Des","NL":"Autom. opslaan: Uit","PL":"Autozapis: Wył.","TR":"Oto-kaydet: Kapalı","VI":"Tự lưu: Tắt","RU":"Автосохр.: Выкл","UK":"Автозбер.: Вимк","JA":"自動保存: オフ","KO":"자동저장: 꺼짐","AR":"حفظ تلقائي: إيقاف"},
 # ── viewport toolbar ──
 "Viewport":    {"ZH":"视口","ES":"Vista","FR":"Fenêtre","DE":"Ansichtsfenster","IT":"Viewport","PT":"Viewport","NL":"Viewport","PL":"Widok 3D","TR":"Görünüm","VI":"Khung nhìn","RU":"Окно","UK":"Область","JA":"ビューポート","KO":"뷰포트","AR":"المشهد"},
 "Move":        {"ZH":"移动","ES":"Mover","FR":"Déplacer","DE":"Bewegen","IT":"Sposta","PT":"Mover","NL":"Verplaatsen","PL":"Przesuń","TR":"Taşı","VI":"Di chuyển","RU":"Перемест.","UK":"Переміст.","JA":"移動","KO":"이동","AR":"تحريك"},
 "Rotate":      {"ZH":"旋转","ES":"Rotar","FR":"Pivoter","DE":"Drehen","IT":"Ruota","PT":"Girar","NL":"Draaien","PL":"Obróć","TR":"Döndür","VI":"Xoay","RU":"Поворот","UK":"Оберт.","JA":"回転","KO":"회전","AR":"تدوير"},
 "Scale":       {"ZH":"缩放","ES":"Escalar","FR":"Échelle","DE":"Skalieren","IT":"Scala","PT":"Escalar","NL":"Schalen","PL":"Skaluj","TR":"Ölçekle","VI":"Tỉ lệ","RU":"Масштаб","UK":"Масштаб","JA":"拡大縮小","KO":"크기","AR":"تحجيم"},
 "Local":       {"ZH":"本地","ES":"Local","FR":"Local","DE":"Lokal","IT":"Locale","PT":"Local","NL":"Lokaal","PL":"Lokalny","TR":"Yerel","VI":"Cục bộ","RU":"Локал.","UK":"Локал.","JA":"ローカル","KO":"로컬","AR":"محلي"},
 "Global":      {"ZH":"全局","ES":"Global","FR":"Global","DE":"Global","IT":"Globale","PT":"Global","NL":"Globaal","PL":"Globalny","TR":"Küresel","VI":"Toàn cục","RU":"Глобал.","UK":"Глобал.","JA":"グローバル","KO":"글로벌","AR":"عام"},
 "Spd":         {"ZH":"速度","ES":"Vel","FR":"Vit","DE":"Geschw","IT":"Vel","PT":"Vel","NL":"Snelh","PL":"Prędk","TR":"Hız","VI":"Tốc độ","RU":"Скор","UK":"Швид","JA":"速度","KO":"속도","AR":"سرعة"},
 "X-ray":       {"ZH":"透视","ES":"Rayos X","FR":"Rayons X","DE":"Röntgen","IT":"Raggi X","PT":"Raio-X","NL":"Röntgen","PL":"Prześwit","TR":"Röntgen","VI":"Xuyên thấu","RU":"Рентген","UK":"Рентген","JA":"透視","KO":"투시","AR":"أشعة سينية"},
 "Audio: On":   {"ZH":"音频: 开","ES":"Audio: Sí","FR":"Audio : Oui","DE":"Audio: An","IT":"Audio: Sì","PT":"Áudio: Lig","NL":"Audio: Aan","PL":"Dźwięk: Wł.","TR":"Ses: Açık","VI":"Âm thanh: Bật","RU":"Звук: Вкл","UK":"Звук: Увім","JA":"音声: オン","KO":"오디오: 켜짐","AR":"الصوت: تشغيل"},
 "Audio: Off":  {"ZH":"音频: 关","ES":"Audio: No","FR":"Audio : Non","DE":"Audio: Aus","IT":"Audio: No","PT":"Áudio: Des","NL":"Audio: Uit","PL":"Dźwięk: Wył.","TR":"Ses: Kapalı","VI":"Âm thanh: Tắt","RU":"Звук: Выкл","UK":"Звук: Вимк","JA":"音声: オフ","KO":"오디오: 꺼짐","AR":"الصوت: إيقاف"},
 "Pin: On":     {"ZH":"固定: 开","ES":"Fijar: Sí","FR":"Épingler : Oui","DE":"Anheften: An","IT":"Fissa: Sì","PT":"Fixar: Lig","NL":"Vastzetten: Aan","PL":"Przypnij: Wł.","TR":"Sabitle: Açık","VI":"Ghim: Bật","RU":"Закреп.: Вкл","UK":"Закріп.: Увім","JA":"固定: オン","KO":"고정: 켜짐","AR":"تثبيت: تشغيل"},
 "Pin: Off":    {"ZH":"固定: 关","ES":"Fijar: No","FR":"Épingler : Non","DE":"Anheften: Aus","IT":"Fissa: No","PT":"Fixar: Des","NL":"Vastzetten: Uit","PL":"Przypnij: Wył.","TR":"Sabitle: Kapalı","VI":"Ghim: Tắt","RU":"Закреп.: Выкл","UK":"Закріп.: Вимк","JA":"固定: オフ","KO":"고정: 꺼짐","AR":"تثبيت: إيقاف"},
 # ── outliner ──
 "Outliner":    {"ZH":"大纲","ES":"Esquema","FR":"Structure","DE":"Übersicht","IT":"Struttura","PT":"Estrutura","NL":"Overzicht","PL":"Struktura","TR":"Ana Hat","VI":"Cấu trúc","RU":"Структура","UK":"Структура","JA":"アウトライナ","KO":"아웃라이너","AR":"المخطط"},
 "+ Add":       {"ZH":"+ 添加","ES":"+ Añadir","FR":"+ Ajouter","DE":"+ Hinzuf.","IT":"+ Aggiungi","PT":"+ Adicionar","NL":"+ Toevoegen","PL":"+ Dodaj","TR":"+ Ekle","VI":"+ Thêm","RU":"+ Добав.","UK":"+ Додати","JA":"+ 追加","KO":"+ 추가","AR":"+ إضافة"},
 "Search":      {"ZH":"搜索","ES":"Buscar","FR":"Rechercher","DE":"Suchen","IT":"Cerca","PT":"Buscar","NL":"Zoeken","PL":"Szukaj","TR":"Ara","VI":"Tìm","RU":"Поиск","UK":"Пошук","JA":"検索","KO":"검색","AR":"بحث"},
 "(no selection)": {"ZH":"(未选择)","ES":"(sin selección)","FR":"(aucune sélection)","DE":"(keine Auswahl)","IT":"(nessuna selezione)","PT":"(sem seleção)","NL":"(geen selectie)","PL":"(brak wyboru)","TR":"(seçim yok)","VI":"(chưa chọn)","RU":"(нет выбора)","UK":"(немає вибору)","JA":"(選択なし)","KO":"(선택 없음)","AR":"(لا تحديد)"},
 # ── tabs ──
 "Material":    {"ZH":"材质","ES":"Material","FR":"Matériau","DE":"Material","IT":"Materiale","PT":"Material","NL":"Materiaal","PL":"Materiał","TR":"Malzeme","VI":"Vật liệu","RU":"Материал","UK":"Матеріал","JA":"マテリアル","KO":"머티리얼","AR":"خامة"},
 "Anim":        {"ZH":"动画","ES":"Anim","FR":"Anim","DE":"Anim","IT":"Anim","PT":"Anim","NL":"Anim","PL":"Anim","TR":"Anim","VI":"Hoạt ảnh","RU":"Аним","UK":"Анім","JA":"アニメ","KO":"애니메이션","AR":"حركة"},
 "Physics":     {"ZH":"物理","ES":"Física","FR":"Physique","DE":"Physik","IT":"Fisica","PT":"Física","NL":"Fysica","PL":"Fizyka","TR":"Fizik","VI":"Vật lý","RU":"Физика","UK":"Фізика","JA":"物理","KO":"물리","AR":"فيزياء"},
 "Scene":       {"ZH":"场景","ES":"Escena","FR":"Scène","DE":"Szene","IT":"Scena","PT":"Cena","NL":"Scène","PL":"Scena","TR":"Sahne","VI":"Cảnh","RU":"Сцена","UK":"Сцена","JA":"シーン","KO":"씬","AR":"مشهد"},
 "Cook":        {"ZH":"烘焙","ES":"Compilar","FR":"Compiler","DE":"Erstellen","IT":"Compila","PT":"Gerar","NL":"Bakken","PL":"Buduj","TR":"Pişir","VI":"Nướng","RU":"Сборка","UK":"Збірка","JA":"ビルド","KO":"구우기","AR":"إنشاء"},
 # ── timeline ──
 "Timeline":    {"ZH":"时间轴","ES":"Línea de tiempo","FR":"Chronologie","DE":"Zeitleiste","IT":"Timeline","PT":"Linha do tempo","NL":"Tijdlijn","PL":"Oś czasu","TR":"Zaman çizelgesi","VI":"Dòng thời gian","RU":"Шкала","UK":"Час. шкала","JA":"タイムライン","KO":"타임라인","AR":"الجدول الزمني"},
 "Pause":       {"ZH":"暂停","ES":"Pausa","FR":"Pause","DE":"Pause","IT":"Pausa","PT":"Pausar","NL":"Pauze","PL":"Pauza","TR":"Duraklat","VI":"Tạm dừng","RU":"Пауза","UK":"Пауза","JA":"一時停止","KO":"일시정지","AR":"إيقاف مؤقت"},
 "Play":        {"ZH":"播放","ES":"Reproducir","FR":"Lecture","DE":"Abspielen","IT":"Riproduci","PT":"Reproduzir","NL":"Afspelen","PL":"Odtwórz","TR":"Oynat","VI":"Phát","RU":"Воспр.","UK":"Грати","JA":"再生","KO":"재생","AR":"تشغيل"},
 "Quest: on":   {"ZH":"Quest: 开","ES":"Quest: Sí","FR":"Quest : Oui","DE":"Quest: An","IT":"Quest: Sì","PT":"Quest: Lig","NL":"Quest: Aan","PL":"Quest: Wł.","TR":"Quest: Açık","VI":"Quest: Bật","RU":"Quest: Вкл","UK":"Quest: Увім","JA":"Quest: オン","KO":"Quest: 켜짐","AR":"Quest: تشغيل"},
 "Quest: off":  {"ZH":"Quest: 关","ES":"Quest: No","FR":"Quest : Non","DE":"Quest: Aus","IT":"Quest: No","PT":"Quest: Des","NL":"Quest: Uit","PL":"Quest: Wył.","TR":"Quest: Kapalı","VI":"Quest: Tắt","RU":"Quest: Выкл","UK":"Quest: Вимк","JA":"Quest: オフ","KO":"Quest: 꺼짐","AR":"Quest: إيقاف"},
 # ── cook panel ──
 "Install to headset after cook (auto)": {"ZH":"烘焙后自动安装到头显","ES":"Instalar en el visor tras compilar (auto)","FR":"Installer sur le casque après compilation (auto)","DE":"Nach dem Erstellen aufs Headset (auto)","IT":"Installa sul visore dopo la compilazione (auto)","PT":"Instalar no headset após gerar (auto)","NL":"Na het bakken op headset zetten (auto)","PL":"Zainstaluj na goglach po zbudowaniu (auto)","TR":"Pişirdikten sonra başlığa kur (oto)","VI":"Cài vào kính sau khi nướng (tự động)","RU":"Установить на шлем после сборки (авто)","UK":"Встановити на шолом після збірки (авто)","JA":"ビルド後にヘッドセットへ自動インストール","KO":"구운 후 헤드셋에 자동 설치","AR":"تثبيت على السماعة بعد الإنشاء (تلقائي)"},
 "COOK  +  SIGN": {"ZH":"烘焙 + 签名","ES":"COMPILAR + FIRMAR","FR":"COMPILER + SIGNER","DE":"ERSTELLEN + SIGNIEREN","IT":"COMPILA + FIRMA","PT":"GERAR + ASSINAR","NL":"BAKKEN + ONDERTEKENEN","PL":"BUDUJ + PODPISZ","TR":"PIŞIR + İMZALA","VI":"NƯỚNG + KÝ","RU":"СБОРКА + ПОДПИСЬ","UK":"ЗБІРКА + ПІДПИС","JA":"ビルド + 署名","KO":"구우기 + 서명","AR":"إنشاء + توقيع"},
 "COOK + SIGN + INSTALL": {"ZH":"烘焙 + 签名 + 安装","ES":"COMPILAR + FIRMAR + INSTALAR","FR":"COMPILER + SIGNER + INSTALLER","DE":"ERSTELLEN + SIGNIEREN + INSTALL.","IT":"COMPILA + FIRMA + INSTALLA","PT":"GERAR + ASSINAR + INSTALAR","NL":"BAKKEN + ONDERTEK. + INSTALL.","PL":"BUDUJ + PODPISZ + ZAINSTALUJ","TR":"PIŞIR + İMZALA + KUR","VI":"NƯỚNG + KÝ + CÀI","RU":"СБОРКА + ПОДП. + УСТАН.","UK":"ЗБІРКА + ПІДП. + ВСТАН.","JA":"ビルド + 署名 + インストール","KO":"구우기 + 서명 + 설치","AR":"إنشاء + توقيع + تثبيت"},
 "Far clip (m)": {"ZH":"远裁剪 (m)","ES":"Plano lejano (m)","FR":"Plan lointain (m)","DE":"Ferne Ebene (m)","IT":"Piano lontano (m)","PT":"Plano distante (m)","NL":"Verre clip (m)","PL":"Daleki plan (m)","TR":"Uzak kesme (m)","VI":"Cắt xa (m)","RU":"Дальняя (m)","UK":"Далека (m)","JA":"遠クリップ (m)","KO":"먼 클립 (m)","AR":"القطع البعيد (m)"},
 "Connect":     {"ZH":"连接","ES":"Conectar","FR":"Connecter","DE":"Verbinden","IT":"Connetti","PT":"Conectar","NL":"Verbinden","PL":"Połącz","TR":"Bağlan","VI":"Kết nối","RU":"Подключ.","UK":"Під'єдн.","JA":"接続","KO":"연결","AR":"اتصال"},
 "Device":      {"ZH":"设备","ES":"Dispositivo","FR":"Appareil","DE":"Gerät","IT":"Dispositivo","PT":"Dispositivo","NL":"Apparaat","PL":"Urządzenie","TR":"Cihaz","VI":"Thiết bị","RU":"Устройство","UK":"Пристрій","JA":"デバイス","KO":"장치","AR":"جهاز"},
 "Distance fog (preview + cook)": {"ZH":"距离雾 (预览 + 烘焙)","ES":"Niebla de distancia (vista + compil.)","FR":"Brouillard (aperçu + compil.)","DE":"Distanznebel (Vorschau + Erstellen)","IT":"Nebbia distanza (anteprima + compil.)","PT":"Neblina de distância (prévia + gerar)","NL":"Afstandsmist (voorbeeld + bakken)","PL":"Mgła dystansu (podgląd + budowa)","TR":"Mesafe sisi (önizleme + pişirme)","VI":"Sương xa (xem trước + nướng)","RU":"Туман (превью + сборка)","UK":"Туман (перегляд + збірка)","JA":"距離フォグ (プレビュー + ビルド)","KO":"거리 안개 (미리보기 + 구우기)","AR":"ضباب المسافة (معاينة + إنشاء)"},
 "Restore original Haven 2025": {"ZH":"恢复原版 Haven 2025","ES":"Restaurar Haven 2025 original","FR":"Restaurer le Haven 2025 d'origine","DE":"Original Haven 2025 wiederherst.","IT":"Ripristina Haven 2025 originale","PT":"Restaurar Haven 2025 original","NL":"Originele Haven 2025 herstellen","PL":"Przywróć oryg. Haven 2025","TR":"Orijinal Haven 2025'i geri yükle","VI":"Khôi phục Haven 2025 gốc","RU":"Восст. ориг. Haven 2025","UK":"Відновити ориг. Haven 2025","JA":"元の Haven 2025 を復元","KO":"원본 Haven 2025 복원","AR":"استعادة Haven 2025 الأصلي"},
 "INSTALL ONLY  (no re-cook if unchanged)": {"ZH":"仅安装 (未改动则不重烤)","ES":"SOLO INSTALAR (sin recompilar si no cambió)","FR":"INSTALLER SEUL. (pas de recompil. si inchangé)","DE":"NUR INSTALL. (kein Neu-Erstellen)","IT":"SOLO INSTALLA (nessuna ricompil.)","PT":"SÓ INSTALAR (sem regerar)","NL":"ALLEEN INSTALL. (geen herbak)","PL":"TYLKO INSTALUJ (bez przebudowy)","TR":"SADECE KUR (değişmediyse yeniden pişirme yok)","VI":"CHỈ CÀI (không nướng lại)","RU":"ТОЛЬКО УСТАН. (без пересборки)","UK":"ЛИШЕ ВСТАН. (без перезбірки)","JA":"インストールのみ (変更なしなら再ビルドなし)","KO":"설치만 (변경 없으면 재구우기 안 함)","AR":"تثبيت فقط (بدون إعادة إنشاء)"},
 "Shell restart after install: AUTO (only if no root)": {"ZH":"安装后重启 Shell: 自动 (仅无 root 时)","ES":"Reiniciar shell tras instalar: AUTO (solo sin root)","FR":"Redémarrer shell après install : AUTO (sans root)","DE":"Shell-Neustart nach Install: AUTO (nur ohne Root)","IT":"Riavvio shell dopo install: AUTO (solo senza root)","PT":"Reiniciar shell após instalar: AUTO (só sem root)","NL":"Shell herstarten na install: AUTO (alleen zonder root)","PL":"Restart shella po instalacji: AUTO (tylko bez root)","TR":"Kurulumdan sonra shell yeniden başlat: OTO (yalnız root yoksa)","VI":"Khởi động lại shell sau cài: TỰ ĐỘNG (chỉ khi không root)","RU":"Перезапуск shell после уст.: АВТО (без root)","UK":"Перезапуск shell після вст.: АВТО (без root)","JA":"インストール後のshell再起動: 自動 (rootなしのみ)","KO":"설치 후 shell 재시작: 자동 (root 없을 때만)","AR":"إعادة تشغيل shell بعد التثبيت: تلقائي (بدون root فقط)"},
 "Shell restart after install: ALWAYS": {"ZH":"安装后重启 Shell: 总是","ES":"Reiniciar shell tras instalar: SIEMPRE","FR":"Redémarrer shell après install : TOUJOURS","DE":"Shell-Neustart nach Install: IMMER","IT":"Riavvio shell dopo install: SEMPRE","PT":"Reiniciar shell após instalar: SEMPRE","NL":"Shell herstarten na install: ALTIJD","PL":"Restart shella po instalacji: ZAWSZE","TR":"Kurulumdan sonra shell yeniden başlat: HER ZAMAN","VI":"Khởi động lại shell sau cài: LUÔN","RU":"Перезапуск shell после уст.: ВСЕГДА","UK":"Перезапуск shell після вст.: ЗАВЖДИ","JA":"インストール後のshell再起動: 常に","KO":"설치 후 shell 재시작: 항상","AR":"إعادة تشغيل shell بعد التثبيت: دائمًا"},
 "Shell restart after install: NEVER": {"ZH":"安装后重启 Shell: 从不","ES":"Reiniciar shell tras instalar: NUNCA","FR":"Redémarrer shell après install : JAMAIS","DE":"Shell-Neustart nach Install: NIE","IT":"Riavvio shell dopo install: MAI","PT":"Reiniciar shell após instalar: NUNCA","NL":"Shell herstarten na install: NOOIT","PL":"Restart shella po instalacji: NIGDY","TR":"Kurulumdan sonra shell yeniden başlat: ASLA","VI":"Khởi động lại shell sau cài: KHÔNG BAO GIỜ","RU":"Перезапуск shell после уст.: НИКОГДА","UK":"Перезапуск shell після вст.: НІКОЛИ","JA":"インストール後のshell再起動: しない","KO":"설치 후 shell 재시작: 안 함","AR":"إعادة تشغيل shell بعد التثبيت: أبدًا"},
 # ── common ──
 "Language":    {"ZH":"语言","ES":"Idioma","FR":"Langue","DE":"Sprache","IT":"Lingua","PT":"Idioma","NL":"Taal","PL":"Język","TR":"Dil","VI":"Ngôn ngữ","RU":"Язык","UK":"Мова","JA":"言語","KO":"언어","AR":"اللغة"},
 "UI language": {"ZH":"界面语言","ES":"Idioma de la interfaz","FR":"Langue de l'interface","DE":"UI-Sprache","IT":"Lingua interfaccia","PT":"Idioma da interface","NL":"UI-taal","PL":"Język interfejsu","TR":"Arayüz dili","VI":"Ngôn ngữ giao diện","RU":"Язык интерфейса","UK":"Мова інтерфейсу","JA":"UI言語","KO":"UI 언어","AR":"لغة الواجهة"},
 "Undo":        {"ZH":"撤销","ES":"Deshacer","FR":"Annuler","DE":"Rückgängig","IT":"Annulla","PT":"Desfazer","NL":"Ongedaan","PL":"Cofnij","TR":"Geri Al","VI":"Hoàn tác","RU":"Отмена","UK":"Скасув.","JA":"元に戻す","KO":"실행 취소","AR":"تراجع"},
 "Redo":        {"ZH":"重做","ES":"Rehacer","FR":"Rétablir","DE":"Wiederholen","IT":"Ripeti","PT":"Refazer","NL":"Opnieuw","PL":"Ponów","TR":"Yinele","VI":"Làm lại","RU":"Повтор","UK":"Повтор","JA":"やり直し","KO":"다시 실행","AR":"إعادة"},
 "Delete":      {"ZH":"删除","ES":"Eliminar","FR":"Supprimer","DE":"Löschen","IT":"Elimina","PT":"Excluir","NL":"Verwijderen","PL":"Usuń","TR":"Sil","VI":"Xóa","RU":"Удалить","UK":"Видалити","JA":"削除","KO":"삭제","AR":"حذف"},
 "Duplicate":   {"ZH":"复制","ES":"Duplicar","FR":"Dupliquer","DE":"Duplizieren","IT":"Duplica","PT":"Duplicar","NL":"Dupliceren","PL":"Powiel","TR":"Çoğalt","VI":"Nhân bản","RU":"Дублировать","UK":"Дублювати","JA":"複製","KO":"복제","AR":"تكرار"},
 "Hide":        {"ZH":"隐藏","ES":"Ocultar","FR":"Masquer","DE":"Ausblenden","IT":"Nascondi","PT":"Ocultar","NL":"Verbergen","PL":"Ukryj","TR":"Gizle","VI":"Ẩn","RU":"Скрыть","UK":"Сховати","JA":"非表示","KO":"숨기기","AR":"إخفاء"},
 "Show":        {"ZH":"显示","ES":"Mostrar","FR":"Afficher","DE":"Anzeigen","IT":"Mostra","PT":"Mostrar","NL":"Tonen","PL":"Pokaż","TR":"Göster","VI":"Hiện","RU":"Показать","UK":"Показати","JA":"表示","KO":"보이기","AR":"إظهار"},
 "Reset":       {"ZH":"重置","ES":"Restablecer","FR":"Réinitialiser","DE":"Zurücksetzen","IT":"Reimposta","PT":"Redefinir","NL":"Reset","PL":"Resetuj","TR":"Sıfırla","VI":"Đặt lại","RU":"Сброс","UK":"Скинути","JA":"リセット","KO":"초기화","AR":"إعادة ضبط"},
 "Position":    {"ZH":"位置","ES":"Posición","FR":"Position","DE":"Position","IT":"Posizione","PT":"Posição","NL":"Positie","PL":"Pozycja","TR":"Konum","VI":"Vị trí","RU":"Позиция","UK":"Позиція","JA":"位置","KO":"위치","AR":"الموضع"},
 "Rotation":    {"ZH":"旋转","ES":"Rotación","FR":"Rotation","DE":"Rotation","IT":"Rotazione","PT":"Rotação","NL":"Rotatie","PL":"Obrót","TR":"Dönüş","VI":"Xoay","RU":"Поворот","UK":"Обертання","JA":"回転","KO":"회전","AR":"تدوير"},
 "Color":       {"ZH":"颜色","ES":"Color","FR":"Couleur","DE":"Farbe","IT":"Colore","PT":"Cor","NL":"Kleur","PL":"Kolor","TR":"Renk","VI":"Màu","RU":"Цвет","UK":"Колір","JA":"色","KO":"색상","AR":"اللون"},
 "Cancel":      {"ZH":"取消","ES":"Cancelar","FR":"Annuler","DE":"Abbrechen","IT":"Annulla","PT":"Cancelar","NL":"Annuleren","PL":"Anuluj","TR":"İptal","VI":"Hủy","RU":"Отмена","UK":"Скасувати","JA":"キャンセル","KO":"취소","AR":"إلغاء"},
 "Clear":       {"ZH":"清除","ES":"Limpiar","FR":"Effacer","DE":"Löschen","IT":"Cancella","PT":"Limpar","NL":"Wissen","PL":"Wyczyść","TR":"Temizle","VI":"Xóa","RU":"Очистить","UK":"Очистити","JA":"クリア","KO":"지우기","AR":"مسح"},
 "Browse":      {"ZH":"浏览","ES":"Examinar","FR":"Parcourir","DE":"Durchsuchen","IT":"Sfoglia","PT":"Procurar","NL":"Bladeren","PL":"Przeglądaj","TR":"Gözat","VI":"Duyệt","RU":"Обзор","UK":"Огляд","JA":"参照","KO":"찾아보기","AR":"تصفح"},
 "Export":      {"ZH":"导出","ES":"Exportar","FR":"Exporter","DE":"Exportieren","IT":"Esporta","PT":"Exportar","NL":"Exporteren","PL":"Eksportuj","TR":"Dışa Aktar","VI":"Xuất","RU":"Экспорт","UK":"Експорт","JA":"エクスポート","KO":"내보내기","AR":"تصدير"},
 "Name":        {"ZH":"名称","ES":"Nombre","FR":"Nom","DE":"Name","IT":"Nome","PT":"Nome","NL":"Naam","PL":"Nazwa","TR":"Ad","VI":"Tên","RU":"Имя","UK":"Ім'я","JA":"名前","KO":"이름","AR":"الاسم"},
 "Radius":      {"ZH":"半径","ES":"Radio","FR":"Rayon","DE":"Radius","IT":"Raggio","PT":"Raio","NL":"Straal","PL":"Promień","TR":"Yarıçap","VI":"Bán kính","RU":"Радиус","UK":"Радіус","JA":"半径","KO":"반지름","AR":"نصف القطر"},
 "Distance":    {"ZH":"距离","ES":"Distancia","FR":"Distance","DE":"Distanz","IT":"Distanza","PT":"Distância","NL":"Afstand","PL":"Odległość","TR":"Mesafe","VI":"Khoảng cách","RU":"Расстояние","UK":"Відстань","JA":"距離","KO":"거리","AR":"المسافة"},
 "Focus":       {"ZH":"聚焦","ES":"Enfocar","FR":"Cibler","DE":"Fokus","IT":"Focus","PT":"Focar","NL":"Focus","PL":"Wyśrodkuj","TR":"Odakla","VI":"Lấy nét","RU":"Фокус","UK":"Фокус","JA":"フォーカス","KO":"초점","AR":"تركيز"},
 "Refresh":     {"ZH":"刷新","ES":"Actualizar","FR":"Actualiser","DE":"Aktualisieren","IT":"Aggiorna","PT":"Atualizar","NL":"Vernieuwen","PL":"Odśwież","TR":"Yenile","VI":"Làm mới","RU":"Обновить","UK":"Оновити","JA":"更新","KO":"새로고침","AR":"تحديث"},
 "Set":         {"ZH":"设置","ES":"Aplicar","FR":"Définir","DE":"Setzen","IT":"Imposta","PT":"Definir","NL":"Instellen","PL":"Ustaw","TR":"Ayarla","VI":"Đặt","RU":"Задать","UK":"Задати","JA":"設定","KO":"설정","AR":"تعيين"},
 "Prefabs":     {"ZH":"预制体","ES":"Prefabs","FR":"Préfabs","DE":"Prefabs","IT":"Prefab","PT":"Prefabs","NL":"Prefabs","PL":"Prefaby","TR":"Prefablar","VI":"Prefab","RU":"Префабы","UK":"Префаби","JA":"プリファブ","KO":"프리합","AR":"قوالب"},
 "Shader":      {"ZH":"着色器","ES":"Sombreador","FR":"Shader","DE":"Shader","IT":"Shader","PT":"Shader","NL":"Shader","PL":"Shader","TR":"Gölgelendirici","VI":"Shader","RU":"Шейдер","UK":"Шейдер","JA":"シェーダー","KO":"셰이더","AR":"مظلل"},
 "Shadow":      {"ZH":"阴影","ES":"Sombra","FR":"Ombre","DE":"Schatten","IT":"Ombra","PT":"Sombra","NL":"Schaduw","PL":"Cień","TR":"Gölge","VI":"Bóng","RU":"Тень","UK":"Тінь","JA":"影","KO":"그림자","AR":"ظل"},
 "Image":       {"ZH":"图像","ES":"Imagen","FR":"Image","DE":"Bild","IT":"Immagine","PT":"Imagem","NL":"Afbeelding","PL":"Obraz","TR":"Görüntü","VI":"Hình ảnh","RU":"Изображ.","UK":"Зображ.","JA":"画像","KO":"이미지","AR":"صورة"},
 "Base texture":{"ZH":"基础贴图","ES":"Textura base","FR":"Texture de base","DE":"Basistextur","IT":"Texture base","PT":"Textura base","NL":"Basistextuur","PL":"Tekstura bazowa","TR":"Temel doku","VI":"Kết cấu gốc","RU":"Баз. текстура","UK":"Баз. текстура","JA":"ベーステクスチャ","KO":"기본 텍스처","AR":"النسيج الأساسي"},
 "Night preview":{"ZH":"夜间预览","ES":"Vista nocturna","FR":"Aperçu nuit","DE":"Nachtvorschau","IT":"Anteprima notte","PT":"Prévia noturna","NL":"Nachtvoorbeeld","PL":"Podgląd nocy","TR":"Gece önizleme","VI":"Xem trước ban đêm","RU":"Ночной режим","UK":"Нічний режим","JA":"夜プレビュー","KO":"야간 미리보기","AR":"معاينة ليلية"},
 "Chair":       {"ZH":"椅子","ES":"Silla","FR":"Chaise","DE":"Stuhl","IT":"Sedia","PT":"Cadeira","NL":"Stoel","PL":"Krzesło","TR":"Sandalye","VI":"Ghế","RU":"Стул","UK":"Стілець","JA":"椅子","KO":"의자","AR":"كرسي"},
 "Skybox / Background": {"ZH":"天空盒 / 背景","ES":"Skybox / Fondo","FR":"Skybox / Arrière-plan","DE":"Skybox / Hintergrund","IT":"Skybox / Sfondo","PT":"Skybox / Fundo","NL":"Skybox / Achtergrond","PL":"Skybox / Tło","TR":"Skybox / Arka plan","VI":"Skybox / Nền","RU":"Небо / Фон","UK":"Небо / Фон","JA":"スカイボックス / 背景","KO":"스카이박스 / 배경","AR":"السماء / الخلفية"},
 "Restore":     {"ZH":"恢复","ES":"Restaurar","FR":"Restaurer","DE":"Wiederherst.","IT":"Ripristina","PT":"Restaurar","NL":"Herstellen","PL":"Przywróć","TR":"Geri Yükle","VI":"Khôi phục","RU":"Восстан.","UK":"Відновити","JA":"復元","KO":"복원","AR":"استعادة"},
 "Dismiss":     {"ZH":"关闭","ES":"Descartar","FR":"Ignorer","DE":"Verwerfen","IT":"Ignora","PT":"Dispensar","NL":"Sluiten","PL":"Odrzuć","TR":"Kapat","VI":"Bỏ qua","RU":"Закрыть","UK":"Закрити","JA":"閉じる","KO":"닫기","AR":"تجاهل"},
 "Warp to spawn":{"ZH":"传送到出生点","ES":"Ir al punto de aparición","FR":"Aller au point d'apparition","DE":"Zum Spawn springen","IT":"Vai allo spawn","PT":"Ir ao ponto de surgimento","NL":"Naar spawn","PL":"Skok do spawnu","TR":"Doğma noktasına atla","VI":"Dịch chuyển đến spawn","RU":"К точке появл.","UK":"До точки появи","JA":"スポーンへワープ","KO":"스폰으로 이동","AR":"الانتقال لنقطة الظهور"},
 "Teleport to camera":{"ZH":"传送到相机","ES":"Teleportar a la cámara","FR":"Téléporter à la caméra","DE":"Zur Kamera teleport.","IT":"Teletrasporta alla camera","PT":"Teleportar para a câmera","NL":"Naar camera teleport.","PL":"Teleportuj do kamery","TR":"Kameraya ışınla","VI":"Dịch chuyển tới camera","RU":"К камере","UK":"До камери","JA":"カメラへテレポート","KO":"카메라로 순간이동","AR":"انتقال للكاميرا"},
 "Reset Transform":{"ZH":"重置变换","ES":"Restablecer transformación","FR":"Réinit. la transformation","DE":"Transform. zurücksetzen","IT":"Reimposta trasformazione","PT":"Redefinir transformação","NL":"Transform. resetten","PL":"Resetuj transformację","TR":"Dönüşümü sıfırla","VI":"Đặt lại biến đổi","RU":"Сброс трансф.","UK":"Скинути трансф.","JA":"変換をリセット","KO":"변환 초기화","AR":"إعادة ضبط التحويل"},
 "Zero rotation":{"ZH":"零旋转","ES":"Rotación cero","FR":"Rotation zéro","DE":"Nullrotation","IT":"Rotazione zero","PT":"Rotação zero","NL":"Nul-rotatie","PL":"Zerowy obrót","TR":"Sıfır dönüş","VI":"Xoay về 0","RU":"Нулевой поворот","UK":"Нульове оберт.","JA":"回転をゼロ","KO":"회전 0","AR":"تدوير صفر"},
}

def esc(s):
    return ''.join('\\x%02x' % b for b in s.encode('utf-8'))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "src", "ui", "i18n.h")
codes = [c for c,_,_ in LANGS]
idx = {c:i for i,c in enumerate(codes)}

lines = []
lines.append("// AUTO-GENERATED by cooker/gen_i18n.py — do not edit. Edit the generator's table + re-run.")
lines.append("// Editor UI localization (GitHub #10). English is the default/key; other languages fall back to")
lines.append("// English per-string. UI strings funnel through ui::Context (textAligned/checkbox/tip) -> tr().")
lines.append("#pragma once")
lines.append("#include <string>")
lines.append("#include <unordered_map>")
lines.append("#include <vector>")
lines.append("#include <cstdint>")
lines.append("")
lines.append("namespace i18n {")
lines.append("")
lines.append("enum Lang { " + ", ".join(codes) + ", LANG_COUNT };")
lines.append("")
lines.append("// native menu name + primary script tag per language")
lines.append("struct LangInfo { const char* code; const char* name; const char* script; };")
langinfo = ", ".join('{"%s","%s","%s"}' % (c, esc(n), s) for c,n,s in LANGS)
lines.append("inline const LangInfo* langs(size_t& n) { static const LangInfo L[] = { %s }; n = LANG_COUNT; return L; }" % langinfo)
lines.append("inline int g_lang = EN;")
lines.append("inline const char* langName(int l) { size_t n; auto* L=langs(n); return (l>=0&&l<(int)n)?L[l].name:L[0].name; }")
lines.append("inline const char* langScript(int l) { size_t n; auto* L=langs(n); return (l>=0&&l<(int)n)?L[l].script:L[0].script; }")
lines.append("")
lines.append("inline unsigned utf8Next(const char*& p) {")
lines.append("    unsigned c = (unsigned char)*p++;")
lines.append("    if (c < 0x80) return c;")
lines.append("    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;")
lines.append("    unsigned cp = c & (0x7F >> (n + 1));")
lines.append("    while (n-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);")
lines.append("    return cp;")
lines.append("}")
lines.append("")
lines.append("// One row per translatable string: t[EN] = english key; t[<lang>] = translation or nullptr (=fall back).")
lines.append("struct Entry { const char* t[LANG_COUNT]; };")
lines.append("inline const Entry* table(size_t& n) {")
lines.append("    static const Entry T[] = {")
for en, tr in T.items():
    cells = ["nullptr"] * len(codes)
    cells[idx["EN"]] = '"%s"' % esc(en)
    for code, txt in tr.items():
        if code in idx and txt:
            cells[idx[code]] = '"%s"' % esc(txt)
    lines.append("        {{ " + ", ".join(cells) + " }},")
lines.append("    };")
lines.append("    n = sizeof(T) / sizeof(T[0]);")
lines.append("    return T;")
lines.append("}")
lines.append("")
lines.append("inline std::unordered_map<std::string, const char*>& activeMap() {")
lines.append("    static std::unordered_map<std::string, const char*> m; static int builtFor = -1;")
lines.append("    if (builtFor != g_lang) { m.clear(); builtFor = g_lang;")
lines.append("        if (g_lang != EN) { size_t n; const Entry* T = table(n);")
lines.append("            for (size_t i=0;i<n;i++) { const char* v = T[i].t[g_lang]; if (v && *v && T[i].t[EN]) m[T[i].t[EN]] = v; } } }")
lines.append("    return m;")
lines.append("}")
lines.append("inline const char* tr(const char* s) {")
lines.append("    if (g_lang == EN || !s || !*s) return s;")
lines.append("    auto& m = activeMap(); auto it = m.find(s); return it == m.end() ? s : it->second;")
lines.append("}")
lines.append("")
lines.append("// Every non-ASCII codepoint any translation uses, so the font atlas bakes exactly those glyphs.")
lines.append("inline void collectExtraCodepoints(std::vector<unsigned>& out) {")
lines.append("    out.clear(); std::unordered_map<unsigned,char> seen;")
lines.append("    auto scan=[&](const char* s){ if(!s)return; const char* p=s; while(*p){ unsigned cp=utf8Next(p); if(cp>=0x80&&!seen.count(cp)){ seen[cp]=1; out.push_back(cp);} } };")
lines.append("    size_t n; const Entry* T = table(n); for (size_t i=0;i<n;i++) for (int l=1;l<LANG_COUNT;l++) scan(T[i].t[l]);")
lines.append("    size_t ln; auto* L=langs(ln); for (size_t i=0;i<ln;i++) scan(L[i].name);")
lines.append("}")
lines.append("")
lines.append("} // namespace i18n")
lines.append("")

out = "\n".join(lines).replace("—", "--")   # em-dash -> ASCII so the header stays pure-ASCII
open(OUT, "w", newline="\n", encoding="ascii").write(out)
print("wrote", OUT, "with", len(LANGS), "languages and", len(T), "strings")
