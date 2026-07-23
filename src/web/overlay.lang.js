// Translations from jellyfin-web/src/strings/
// Generated with:
// jq -n '[inputs | { lang: input_filename | split(".")[0], HeaderConnectToServer: .HeaderConnectToServer, LabelServerHost: .LabelServerHost, LabelServerHostHelp: .LabelServerHostHelp, Connect: .Connect, HeaderConnectionFailure: .HeaderConnectionFailure, MessageUnableToConnectToServer: .MessageUnableToConnectToServer, ButtonGotIt: .ButtonGotIt }]' *.json

const languages = [
  {
    "lang": "af",
    "HeaderConnectToServer": "Konnekteer aan Bediener",
    "LabelServerHost": "Bedieneradres",
    "LabelServerHostHelp": "192.168.1.100:8096 of https://myserver.com",
    "Connect": "Konnekteer",
    "Cancel": "Kanselleer",
    "HeaderConnectionFailure": "Konneksie Fout",
    "MessageUnableToConnectToServer": "Ons kan nie tans aan die gekose bediener koppel nie. Maak seker dit loop en probeer weer.",
    "ButtonGotIt": "Het Dit So"
  },
  {
    "lang": "ar",
    "HeaderConnectToServer": "الاتصال بالخادم",
    "LabelServerHost": "المضيف",
    "LabelServerHostHelp": "192.168.1.100:8096 أو https://myserver.com",
    "Connect": "اتصال",
    "Cancel": "إلغاء",
    "HeaderConnectionFailure": "فشل الاتصال",
    "MessageUnableToConnectToServer": "تعذر الاتصال بالخادم المحدد الآن. يرجى التأكد من أنه قيد التشغيل والمحاولة مرة أخرى.",
    "ButtonGotIt": "فهمت"
  },
  {
    "lang": "as",
    "HeaderConnectToServer": "ছাৰ্ভাৰলৈ সংযোগ কৰক",
    "LabelServerHost": "ছাৰ্ভাৰ ঠিকনা",
    "LabelServerHostHelp": "192.168.1.100:8096 অথবা https://myserver.com",
    "Connect": "সংযোগ কৰক",
    "Cancel": "বাতিল কৰক",
    "HeaderConnectionFailure": "সংযোগ বিফল",
    "MessageUnableToConnectToServer": "আমি এতিয়া নিৰ্বাচিত ছাৰ্ভাৰৰ সৈতে সংযোগ কৰিব পৰা নাই। ই চলি আছে নে নাই নিশ্চিত কৰি পুনৰ চেষ্টা কৰক।",
    "ButtonGotIt": "বুজিলোঁ"
  },
  {
    "lang": "be-by",
    "HeaderConnectToServer": "Падлучыцца да сервера",
    "LabelServerHost": "Вядучы",
    "LabelServerHostHelp": "192.168.1.100:8096 або https://myserver.com",
    "Connect": "Падлучыцца",
    "Cancel": "Адмяніць",
    "HeaderConnectionFailure": "Збой падлучэння",
    "MessageUnableToConnectToServer": "Мы не можам зараз падключыцца да выбранага сервера. Упэўніцеся, што ён запушчаны, і паўтарыце спробу.",
    "ButtonGotIt": "Зразумела"
  },
  {
    "lang": "bg-bg",
    "HeaderConnectToServer": "Свържи се със сървър",
    "LabelServerHost": "Хост",
    "LabelServerHostHelp": "192.168.1.100:8096 или https://myserver.com",
    "Connect": "Свързване",
    "Cancel": "Отмяна",
    "HeaderConnectionFailure": "Проблем при свързване",
    "MessageUnableToConnectToServer": "В момента не можем да се свържем с избрания сървър. Моля, уверете се, че работи и опитайте отново.",
    "ButtonGotIt": "Разбрано"
  },
  {
    "lang": "bn",
    "HeaderConnectToServer": "সার্ভারে সংযোগ করুন",
    "LabelServerHost": "সার্ভারের ঠিকানা",
    "LabelServerHostHelp": "192.168.1.100:8096 অথবা https://myserver.com",
    "Connect": "সংযোগ করুন",
    "Cancel": "বাতিল",
    "HeaderConnectionFailure": "সংযোগ ব্যর্থ হয়েছে",
    "MessageUnableToConnectToServer": "আমরা এখন নির্বাচিত সার্ভারে সংযোগ করতে পারছি না। দয়া করে নিশ্চিত করুন এটি চালু আছে এবং আবার চেষ্টা করুন।",
    "ButtonGotIt": "বুঝেছি"
  },
  {
    "lang": "bn_BD",
    "HeaderConnectToServer": "সার্ভারে সংযোগ করুন",
    "LabelServerHost": "সার্ভারের ঠিকানা",
    "LabelServerHostHelp": "192.168.1.100:8096 অথবা https://myserver.com",
    "Connect": "কানেক্ট",
    "Cancel": "বাতিল",
    "HeaderConnectionFailure": "সংযোগ ব্যর্থ হয়েছে",
    "MessageUnableToConnectToServer": "আমরা এখন নির্বাচিত সার্ভারে সংযোগ করতে পারছি না। দয়া করে নিশ্চিত করুন এটি চালু আছে এবং আবার চেষ্টা করুন।",
    "ButtonGotIt": "বুঝেছি"
  },
  {
    "lang": "ca",
    "HeaderConnectToServer": "Connecta al servidor",
    "LabelServerHost": "Amfitrió",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://myserver.com",
    "Connect": "Connecta",
    "Cancel": "Cancel·la",
    "HeaderConnectionFailure": "Error de connexió",
    "MessageUnableToConnectToServer": "En aquest moment, no es pot connectar amb el servidor seleccionat. Assegureu-vos que estigui funcionant i torneu a intentar-ho.",
    "ButtonGotIt": "Entesos"
  },
  {
    "lang": "ch",
    "HeaderConnectToServer": "Konnektå gi setbidot",
    "LabelServerHost": "Direksion setbidot",
    "LabelServerHostHelp": "192.168.1.100:8096 pat https://myserver.com",
    "Connect": "Konnektå",
    "Cancel": "Kansela",
    "HeaderConnectionFailure": "Mungnga i koneksion",
    "MessageUnableToConnectToServer": "Ti siña ham konnektå gi ayu na setbidot pågo. Asegura na mamamaila ya tåtte un prøba.",
    "ButtonGotIt": "Hu komprende"
  },
  {
    "lang": "cs",
    "HeaderConnectToServer": "Připojit k serveru",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 nebo https://mujserver.cz",
    "Connect": "Připojit",
    "Cancel": "Zrušit",
    "HeaderConnectionFailure": "Připojení selhalo",
    "MessageUnableToConnectToServer": "Nejsme schopni se připojit k vybranému serveru právě teď. Prosím, ujistěte se, že je spuštěn a zkuste to znovu.",
    "ButtonGotIt": "Rozumím"
  },
  {
    "lang": "cy",
    "HeaderConnectToServer": "Cysylltu â gweinydd",
    "LabelServerHost": "Lletywr",
    "LabelServerHostHelp": "192.168.1.100:8096 neu https://myserver.com",
    "Connect": "Cysylltu",
    "Cancel": "Canslo",
    "HeaderConnectionFailure": "Methiant cysylltu",
    "MessageUnableToConnectToServer": "Ni allwn gysylltu â’r gweinydd a ddewiswyd ar hyn o bryd. Gwnewch yn siŵr ei fod yn rhedeg a rhowch gynnig arall arni.",
    "ButtonGotIt": "Dyna Fe"
  },
  {
    "lang": "da",
    "HeaderConnectToServer": "Forbind til server",
    "LabelServerHost": "Vært",
    "LabelServerHostHelp": "F. eks: 192.168.1.100:8096 eller https://myserver.com",
    "Connect": "Forbind",
    "Cancel": "Annullér",
    "HeaderConnectionFailure": "Forbindelsesfejl",
    "MessageUnableToConnectToServer": "Vi kan ikke forbinde til den valgte server på nuværende tidspunkt. Sikr dig venligst at serveren kører og prøv igen.",
    "ButtonGotIt": "Forstået"
  },
  {
    "lang": "de",
    "HeaderConnectToServer": "Mit Server verbinden",
    "LabelServerHost": "Adresse",
    "LabelServerHostHelp": "192.168.1.100:8096 oder https://myserver.com",
    "Connect": "Verbinden",
    "Cancel": "Abbrechen",
    "HeaderConnectionFailure": "Verbindungsfehler",
    "MessageUnableToConnectToServer": "Wir können gerade keine Verbindung zum gewählten Server herstellen. Bitte stelle sicher, dass dieser läuft und versuche es erneut.",
    "ButtonGotIt": "Verstanden"
  },
  {
    "lang": "el",
    "HeaderConnectToServer": "Σύνδεση στον Διακομιστή",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 ή https://myserver.com",
    "Connect": "Σύνδεση",
    "Cancel": "Ακύρωση",
    "HeaderConnectionFailure": "Αποτυχία σύνδεσης",
    "MessageUnableToConnectToServer": "Δεν είναι δυνατή η σύνδεση με τον επιλεγμένο διακομιστή αυτή τη στιγμή. Βεβαιωθείτε ότι εκτελείται και προσπαθήστε ξανά.",
    "ButtonGotIt": "Το κατάλαβα"
  },
  {
    "lang": "en-gb",
    "HeaderConnectToServer": "Connect to Server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 or https://myserver.com",
    "Connect": "Connect",
    "Cancel": "Cancel",
    "HeaderConnectionFailure": "Connection Failure",
    "MessageUnableToConnectToServer": "We're unable to connect to the selected server right now. Please ensure it is running and try again.",
    "ButtonGotIt": "Got It"
  },
  {
    "lang": "en-us",
    "HeaderConnectToServer": "Connect to Server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 or https://myserver.com",
    "Connect": "Connect",
    "Cancel": "Cancel",
    "HeaderConnectionFailure": "Connection Failure",
    "MessageUnableToConnectToServer": "We're unable to connect to the selected server right now. Please ensure it is running and try again.",
    "ButtonGotIt": "Got It"
  },
  {
    "lang": "eo",
    "HeaderConnectToServer": "Konekti al Servilo",
    "LabelServerHost": "Gastigo",
    "LabelServerHostHelp": "192.168.1.100:8096 aŭ https://myserver.com",
    "Connect": "Konektu",
    "Cancel": "Rezignu",
    "HeaderConnectionFailure": "Konekto Malsukcesis",
    "MessageUnableToConnectToServer": "Ni ne povas konektiĝi al la elektita servilo nun. Certigi, ke ĝi funkcias kaj provi denove.",
    "ButtonGotIt": "Kompreneblas"
  },
  {
    "lang": "es-ar",
    "HeaderConnectToServer": "Conectar al servidor",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://miservidor.com",
    "Connect": "Conectar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Conexión fallida",
    "MessageUnableToConnectToServer": "No podemos conectarnos al servidor seleccionado en este momento. Asegurate de que se esté ejecutando y probá de nuevo.",
    "ButtonGotIt": "Lo entendí"
  },
  {
    "lang": "es-mx",
    "HeaderConnectToServer": "Conectarse al servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://miservidor.com",
    "Connect": "Conectar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Falla de conexión",
    "MessageUnableToConnectToServer": "No podemos conectarnos al servidor seleccionado en este momento. Por favor, asegúrate de que está funcionando e inténtalo de nuevo.",
    "ButtonGotIt": "Hecho"
  },
  {
    "lang": "es",
    "HeaderConnectToServer": "Conectar al servidor",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://miservidor.com",
    "Connect": "Conectar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Fallo de conexión",
    "MessageUnableToConnectToServer": "No podemos conectar con el servidor seleccionado ahora mismo. Por favor, asegúrate de que esta funcionando e inténtalo otra vez.",
    "ButtonGotIt": "Entendido"
  },
  {
    "lang": "es_419",
    "HeaderConnectToServer": "Conectarse al servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://miservidor.com",
    "Connect": "Conectar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Falla de conexión",
    "MessageUnableToConnectToServer": "No podemos conectarnos al servidor seleccionado en este momento. Por favor, asegúrate de que está funcionando e inténtalo de nuevo.",
    "ButtonGotIt": "Hecho"
  },
  {
    "lang": "es_DO",
    "HeaderConnectToServer": "Conectar al servidor",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://miservidor.com",
    "Connect": "Conectar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Fallo de conexión",
    "MessageUnableToConnectToServer": "No podemos conectar con el servidor seleccionado ahora mismo. Por favor, asegúrate de que esta funcionando e inténtalo otra vez.",
    "ButtonGotIt": "Entendido"
  },
  {
    "lang": "et",
    "HeaderConnectToServer": "Ühendu serveriga",
    "LabelServerHost": "Peremeesmasin",
    "LabelServerHostHelp": "192.168.1.100:8096 või https://myserver.com",
    "Connect": "Ühenda",
    "Cancel": "Tühista",
    "HeaderConnectionFailure": "Ühenduse tõrge",
    "MessageUnableToConnectToServer": "Me ei saa praegu valitud serveriga ühendust. Veendu, et see töötab ja proovi uuesti.",
    "ButtonGotIt": "Selge"
  },
  {
    "lang": "eu",
    "HeaderConnectToServer": "Zerbitzariari konektatu",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100: 8096 edo https://miservidor.com",
    "Connect": "Konektatu",
    "Cancel": "Ezeztatu",
    "HeaderConnectionFailure": "Konexio-akatsa",
    "MessageUnableToConnectToServer": "Ezin dugu une honetan hautatutako zerbitzariarekin konektatu. Mesedez, ziurtatu funtzionatzen ari dela eta saiatu berriro.",
    "ButtonGotIt": "Ulertua"
  },
  {
    "lang": "fa",
    "HeaderConnectToServer": "اتصال به سرور",
    "LabelServerHost": "میزبان",
    "LabelServerHostHelp": "192.168.1.100:8096 یا https://myserver.com",
    "Connect": "اتصال",
    "Cancel": "لغو کردن",
    "HeaderConnectionFailure": "عدم اتصال",
    "MessageUnableToConnectToServer": "در حال حاضر نمی‌توانیم به سرور انتخاب‌شده متصل شویم. لطفاً مطمئن شوید که سرور در حال اجراست و دوباره تلاش کنید.",
    "ButtonGotIt": "متوجه شدم"
  },
  {
    "lang": "fi",
    "HeaderConnectToServer": "Yhdistä palvelimeen",
    "LabelServerHost": "Isäntä",
    "LabelServerHostHelp": "192.168.1.100:8096 tai https://myserver.com",
    "Connect": "Yhdistä",
    "Cancel": "Peruuta",
    "HeaderConnectionFailure": "Yhteys epäonnistui",
    "MessageUnableToConnectToServer": "Valittuun palvelimeen yhdistäminen epäonnistui. Tarkista, että se on päällä ja yritä uudestaan.",
    "ButtonGotIt": "Selvä"
  },
  {
    "lang": "fil",
    "HeaderConnectToServer": "Kumonekta sa Server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://myserver.com",
    "Connect": "Kumonekta",
    "Cancel": "Kanselahin",
    "HeaderConnectionFailure": "Nag-fail ang koneksyon",
    "MessageUnableToConnectToServer": "Hindi kami makakonekta sa napiling server sa ngayon. Pakitiyak na ito ay tumatakbo at subukang muli.",
    "ButtonGotIt": "Nakuha ko"
  },
  {
    "lang": "fo",
    "HeaderConnectToServer": "Set samband við ambætara",
    "LabelServerHost": "Ambætarasamband",
    "LabelServerHostHelp": "192.168.1.100:8096 ella https://myserver.com",
    "Connect": "Set samband",
    "Cancel": "Avlýs",
    "HeaderConnectionFailure": "Sambandið miseydnaðist",
    "MessageUnableToConnectToServer": "Vit kunnu ikki seta samband við valda ambætaran beint nú. Tryggja tær, at hann koyrir, og royn aftur.",
    "ButtonGotIt": "Skilt"
  },
  {
    "lang": "fr-ca",
    "HeaderConnectToServer": "Connexion au serveur",
    "LabelServerHost": "Hôte",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://monserveur.com",
    "Connect": "Connexion",
    "Cancel": "Annuler",
    "HeaderConnectionFailure": "Échec de connexion",
    "MessageUnableToConnectToServer": "Impossible de se connecter au serveur sélectionné. Assurez-vous qu'il est opérationnel.",
    "ButtonGotIt": "J'ai compris"
  },
  {
    "lang": "fr",
    "HeaderConnectToServer": "Connexion au serveur",
    "LabelServerHost": "Nom d'hôte",
    "LabelServerHostHelp": "192.168.1.1:8096 ou https://monserveur.com",
    "Connect": "Se connecter",
    "Cancel": "Annuler",
    "HeaderConnectionFailure": "Échec de connexion",
    "MessageUnableToConnectToServer": "Nous sommes dans l'impossibilité de nous connecter au serveur sélectionné. Veuillez vérifier qu'il est opérationnel et réessayez.",
    "ButtonGotIt": "Compris"
  },
  {
    "lang": "ga",
    "HeaderConnectToServer": "Ceangail leis an bhfreastalaí",
    "LabelServerHost": "Óstríomhaire",
    "LabelServerHostHelp": "192.168.1.100:8096 nó https://myserver.com",
    "Connect": "Ceangail",
    "Cancel": "Cuir ar ceal",
    "HeaderConnectionFailure": "Teip an Naisc",
    "MessageUnableToConnectToServer": "Ní féidir linn ceangal leis an bhfreastalaí roghnaithe anois. Cinntigh le do thoil go bhfuil sé ag rith agus bain triail eile as.",
    "ButtonGotIt": "Tá sé agam"
  },
  {
    "lang": "gl",
    "HeaderConnectToServer": "Conectar ao Servidor",
    "LabelServerHost": "Enderezo do servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://myserver.com",
    "Connect": "Conectar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Fallo de Conexión",
    "MessageUnableToConnectToServer": "Non podemos conectar co servidor seleccionado neste momento. Asegúrate de que está en execución e téntao de novo.",
    "ButtonGotIt": "Entendo"
  },
  {
    "lang": "gsw",
    "HeaderConnectToServer": "Mit em Server verbinde",
    "LabelServerHost": "Serveradresse",
    "LabelServerHostHelp": "192.168.1.100:8096 oder https://myserver.com",
    "Connect": "Verbinde",
    "Cancel": "Abbreche",
    "HeaderConnectionFailure": "Verbindig fehlgschlage",
    "MessageUnableToConnectToServer": "Mir chönd grad kei Verbindig zum usgwählte Server herstelle. Stell sicher, dass er lauft, und versuech es nomal.",
    "ButtonGotIt": "Verstande"
  },
  {
    "lang": "gu",
    "HeaderConnectToServer": "સર્વર સાથે જોડાઓ",
    "LabelServerHost": "સર્વર સરનામું",
    "LabelServerHostHelp": "192.168.1.100:8096 અથવા https://myserver.com",
    "Connect": "જોડાઓ",
    "Cancel": "રદ કરો",
    "HeaderConnectionFailure": "કનેક્શન નિષ્ફળ થયું",
    "MessageUnableToConnectToServer": "અમે હાલમાં પસંદ કરેલા સર્વર સાથે જોડાઈ શકતા નથી. કૃપા કરીને ખાતરી કરો કે તે ચાલી રહ્યું છે અને ફરી પ્રયાસ કરો.",
    "ButtonGotIt": "સમજાઈ ગયું"
  },
  {
    "lang": "he",
    "HeaderConnectToServer": "התחבר לשרת",
    "LabelServerHost": "מארח",
    "LabelServerHostHelp": "192.168.1.100:8096 או https://myserver.com",
    "Connect": "התחבר",
    "Cancel": "בטל",
    "HeaderConnectionFailure": "כשל בחיבור",
    "MessageUnableToConnectToServer": "לא ניתן להתחבר לשרת הנבחר כרגע. נא לוודא שהוא רץ ולנסות שוב.",
    "ButtonGotIt": "הבנתי"
  },
  {
    "lang": "hi-in",
    "HeaderConnectToServer": "सर्वर से कनेक्ट करें",
    "LabelServerHost": "सर्वर पता",
    "LabelServerHostHelp": "192.168.1.100:8096 या https://myserver.com",
    "Connect": "कनेक्ट करें",
    "Cancel": "रद्द करना",
    "HeaderConnectionFailure": "कनेक्शन विफल",
    "MessageUnableToConnectToServer": "हम अभी चयनित सर्वर से कनेक्ट नहीं कर पा रहे हैं। कृपया सुनिश्चित करें कि यह चल रहा है और फिर से प्रयास करें।",
    "ButtonGotIt": "समझ गया"
  },
  {
    "lang": "hr",
    "HeaderConnectToServer": "Spoji se na server",
    "LabelServerHost": "Domaćin",
    "LabelServerHostHelp": "192.168.1.100:8096 ili https://myserver.com",
    "Connect": "Spoji se",
    "Cancel": "Odustani",
    "HeaderConnectionFailure": "Neuspjelo spajanje",
    "MessageUnableToConnectToServer": "Nismo u mogućnosti spojiti se na odabrani poslužitelj. Provjerite dali je pokrenut i pokušajte ponovno.",
    "ButtonGotIt": "Shvaćam"
  },
  {
    "lang": "hu",
    "HeaderConnectToServer": "Kapcsolódás a kiszolgálóhoz",
    "LabelServerHost": "Kiszolgáló",
    "LabelServerHostHelp": "192.168.1.100:8096 vagy https://myserver.com",
    "Connect": "Kapcsolódás",
    "Cancel": "Mégse",
    "HeaderConnectionFailure": "Kapcsolathiba",
    "MessageUnableToConnectToServer": "Jelenleg nem tudunk csatlakozni a kiválasztott szerverhez. Győződj meg róla, hogy fut és próbáld meg újra.",
    "ButtonGotIt": "Értettem"
  },
  {
    "lang": "hy",
    "HeaderConnectToServer": "Միանալ սերվերին",
    "LabelServerHost": "Սերվերի հասցե",
    "LabelServerHostHelp": "192.168.1.100:8096 կամ https://myserver.com",
    "Connect": "Միանալ",
    "Cancel": "Չեղարկել",
    "HeaderConnectionFailure": "Միացումը ձախողվեց",
    "MessageUnableToConnectToServer": "Այս պահին չենք կարող միանալ ընտրված սերվերին։ Խնդրում ենք համոզվել, որ այն աշխատում է, և կրկին փորձել։",
    "ButtonGotIt": "Հասկացա"
  },
  {
    "lang": "id",
    "HeaderConnectToServer": "Sambungkan ke server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 atau https://myserver.com",
    "Connect": "Sambung",
    "Cancel": "Batal",
    "HeaderConnectionFailure": "Koneksi Bermasalah",
    "MessageUnableToConnectToServer": "Kami tidak dapat terhubung ke server yang dipilih sekarang. Harap pastikan itu berjalan dan coba lagi.",
    "ButtonGotIt": "Paham"
  },
  {
    "lang": "is-is",
    "HeaderConnectToServer": "Tengjast við þjón",
    "LabelServerHost": "Vistfang þjóns",
    "LabelServerHostHelp": "192.168.1.100:8096 eða https://myserver.com",
    "Connect": "Tengjast",
    "Cancel": "Hætta við",
    "HeaderConnectionFailure": "Tenging mistókst",
    "MessageUnableToConnectToServer": "Við getum ekki tengst völdum þjóni núna. Gakktu úr skugga um að hann sé í gangi og reyndu aftur.",
    "ButtonGotIt": "Skilið"
  },
  {
    "lang": "it",
    "HeaderConnectToServer": "Connettiti al server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://myserver.com",
    "Connect": "Connetti",
    "Cancel": "Annulla",
    "HeaderConnectionFailure": "Errore di connessione",
    "MessageUnableToConnectToServer": "Impossibile connettersi al server selezionato al momento. Assicurati che sia in esecuzione e riprova.",
    "ButtonGotIt": "Ho capito"
  },
  {
    "lang": "ja",
    "HeaderConnectToServer": "サーバーに接続",
    "LabelServerHost": "ホスト",
    "LabelServerHostHelp": "192.168.1.100:8096 又は https://myserver.com",
    "Connect": "接続",
    "Cancel": "キャンセル",
    "HeaderConnectionFailure": "接続失敗",
    "MessageUnableToConnectToServer": "現在、選択されたサーバーへの接続ができません。稼働していることを確認しもう一度やり直してください。",
    "ButtonGotIt": "了解"
  },
  {
    "lang": "jbo",
    "HeaderConnectToServer": "jorne lo samci",
    "LabelServerHost": "lo samci judri",
    "LabelServerHostHelp": "192.168.1.100:8096 .a https://myserver.com",
    "Connect": "jorne",
    "Cancel": "sisti",
    "HeaderConnectionFailure": "lo nu jorne cu fliba",
    "MessageUnableToConnectToServer": "mi na kakne lo nu jorne le se cuxna samci ca ti. ko birti lo nu ri ca ca'o gunka gi'e za'u re'u troci",
    "ButtonGotIt": "je'e"
  },
  {
    "lang": "ka",
    "HeaderConnectToServer": "სერვერთან დაკავშირება",
    "LabelServerHost": "ჰოსტი",
    "LabelServerHostHelp": "192.168.1.100:8096 ან https://myserver.com",
    "Connect": "დაკავშირება",
    "Cancel": "გაუქმება",
    "HeaderConnectionFailure": "კავშირის ჩავარდნა",
    "MessageUnableToConnectToServer": "ამჟამად არჩეულ სერვერთან დაკავშირება ვერ ხერხდება. დარწმუნდით, რომ ის გაშვებულია და სცადეთ ხელახლა.",
    "ButtonGotIt": "გავიგე"
  },
  {
    "lang": "kab",
    "HeaderConnectToServer": "Qqen ɣer uqeddac",
    "LabelServerHost": "Tansa n uqeddac",
    "LabelServerHostHelp": "192.168.1.100:8096 neɣ https://myserver.com",
    "Connect": "Qqen",
    "Cancel": "Sefsex",
    "HeaderConnectionFailure": "Tuqqna tecceḍ",
    "MessageUnableToConnectToServer": "Ur nezmir ara ad neqqen ɣer uqeddac yettwafernen akka tura. Ttxil-k wali ma iteddu, syen ɛreḍ tikelt-nniḍen.",
    "ButtonGotIt": "Gziɣ"
  },
  {
    "lang": "kk",
    "HeaderConnectToServer": "Serverge qosylu",
    "LabelServerHost": "Tüiın",
    "LabelServerHostHelp": "192.168.1.100:8096 nemese https://myserver.com",
    "Connect": "Qosylu",
    "Cancel": "Boldyrmau",
    "HeaderConnectionFailure": "Qosylu sätsız",
    "MessageUnableToConnectToServer": "Tañdalğan serverge qosyluymyz däl qazır mümkın emes. Būl ıske qosylğanyna köz jetkızıñız jäne ärekettı keiın qaitalañyz.",
    "ButtonGotIt": "Tüsınıktı"
  },
  {
    "lang": "kn",
    "HeaderConnectToServer": "ಸರ್ವರ್‌ಗೆ ಸಂಪರ್ಕಿಸಿ",
    "LabelServerHost": "ಸರ್ವರ್ ವಿಳಾಸ",
    "LabelServerHostHelp": "192.168.1.100:8096 ಅಥವಾ https://myserver.com",
    "Connect": "ಸಂಪರ್ಕಿಸಿ",
    "Cancel": "ರದ್ದುಮಾಡಿ",
    "HeaderConnectionFailure": "ಸಂಪರ್ಕ ವಿಫಲವಾಗಿದೆ",
    "MessageUnableToConnectToServer": "ಈಗ ಆಯ್ಕೆ ಮಾಡಿದ ಸರ್ವರ್‌ಗೆ ಸಂಪರ್ಕಿಸಲು ಸಾಧ್ಯವಾಗುತ್ತಿಲ್ಲ. ಅದು ಚಾಲನೆಯಲ್ಲಿದೆ ಎಂದು ಖಚಿತಪಡಿಸಿ ಮತ್ತೆ ಪ್ರಯತ್ನಿಸಿ.",
    "ButtonGotIt": "ಅರ್ಥವಾಯಿತು"
  },
  {
    "lang": "ko",
    "HeaderConnectToServer": "서버 접속",
    "LabelServerHost": "호스트",
    "LabelServerHostHelp": "192.168.1.100:8096 또는 https://myserver.com",
    "Connect": "접속",
    "Cancel": "취소",
    "HeaderConnectionFailure": "연결 실패",
    "MessageUnableToConnectToServer": "선택한 서버에 연결할 수 없습니다. 서버가 실행 중인지 확인후 다시 시도하세요.",
    "ButtonGotIt": "알겠습니다"
  },
  {
    "lang": "kw",
    "HeaderConnectToServer": "Kevrea orth servyer",
    "LabelServerHost": "Trigva an servyer",
    "LabelServerHostHelp": "192.168.1.100:8096 po https://myserver.com",
    "Connect": "Kevrea",
    "Cancel": "Hedhi",
    "HeaderConnectionFailure": "Kevreadh fallis",
    "MessageUnableToConnectToServer": "Ny allwn ni kevrea orth an servyer dewisys lemmyn. Gwrewgh sur bos ev owth oberi hag assayewgh arta.",
    "ButtonGotIt": "Dealladow"
  },
  {
    "lang": "ky",
    "HeaderConnectToServer": "Серверге туташуу",
    "LabelServerHost": "Сервер дареги",
    "LabelServerHostHelp": "192.168.1.100:8096 же https://myserver.com",
    "Connect": "Туташуу",
    "Cancel": "Жокко чыгаруу",
    "HeaderConnectionFailure": "Туташуу ишке ашкан жок",
    "MessageUnableToConnectToServer": "Азыр тандалган серверге туташа албай жатабыз. Ал иштеп жатканын текшерип, кайра аракет кылыңыз.",
    "ButtonGotIt": "Түшүндүм"
  },
  {
    "lang": "lt-lt",
    "HeaderConnectToServer": "Prisijungti prie Serverio",
    "LabelServerHost": "Hostas",
    "LabelServerHostHelp": "192.168.1.100:8096 arba https://manoserveris.lt",
    "Connect": "Prisijungti",
    "Cancel": "Atšaukti",
    "HeaderConnectionFailure": "Prisijungimo klaida",
    "MessageUnableToConnectToServer": "Šiuo metu negalime prisijungti prie pasirinkto serverio. Įsitikinkite, kad jis veikia, ir bandykite dar kartą.",
    "ButtonGotIt": "Supratau"
  },
  {
    "lang": "lv",
    "HeaderConnectToServer": "Pievienoties pie servera",
    "LabelServerHost": "Resursdators",
    "LabelServerHostHelp": "192.168.1.100:8096 vai https://myserver.com",
    "Connect": "Savienot",
    "Cancel": "Atcelt",
    "HeaderConnectionFailure": "Savienojuma kļūda",
    "MessageUnableToConnectToServer": "Mēs pašlaik nevaram sazināties ar izvēlēto serveri. Pārliecinies ka tas strādā, un mēģini vēlreiz.",
    "ButtonGotIt": "Sapratu"
  },
  {
    "lang": "mg",
    "HeaderConnectToServer": "Hifandray amin’ny mpizara",
    "LabelServerHost": "Adiresin’ny mpizara",
    "LabelServerHostHelp": "192.168.1.100:8096 na https://myserver.com",
    "Connect": "Hifandray",
    "Cancel": "Foano",
    "HeaderConnectionFailure": "Tsy nahomby ny fifandraisana",
    "MessageUnableToConnectToServer": "Tsy afaka mifandray amin’ilay mpizara voafantina izahay izao. Hamarino fa mandeha izy ary andramo indray.",
    "ButtonGotIt": "Azoko"
  },
  {
    "lang": "mk",
    "HeaderConnectToServer": "Поврзи се со серверот",
    "LabelServerHost": "Адреса на сервер",
    "LabelServerHostHelp": "192.168.1.100:8096 или https://myserver.com",
    "Connect": "Поврзи",
    "Cancel": "Откажи",
    "HeaderConnectionFailure": "Поврзувањето е неуспешно",
    "MessageUnableToConnectToServer": "Во моментов не можеме да се поврземе со избраниот сервер. Проверете дали работи и обидете се повторно.",
    "ButtonGotIt": "Потврдувам"
  },
  {
    "lang": "ml",
    "HeaderConnectToServer": "സെർവറിലേക്ക് കണക്റ്റുചെയ്യുക",
    "LabelServerHost": "ഹോസ്റ്റ്",
    "LabelServerHostHelp": "192.168.1.100:8096 അല്ലെങ്കിൽ https://myserver.com",
    "Connect": "ബന്ധിപ്പിക്കുക",
    "Cancel": "റദ്ദാക്കുക",
    "HeaderConnectionFailure": "കണക്ഷൻ പരാജയം",
    "MessageUnableToConnectToServer": "തിരഞ്ഞെടുത്ത സെർവറിലേക്ക് ഞങ്ങൾക്ക് ഇപ്പോൾ കണക്റ്റുചെയ്യാൻ കഴിയില്ല. ഇത് പ്രവർത്തിക്കുന്നുവെന്ന് ഉറപ്പാക്കി വീണ്ടും ശ്രമിക്കുക.",
    "ButtonGotIt": "മനസ്സിലായി"
  },
  {
    "lang": "mn",
    "HeaderConnectToServer": "Серверт холбогдох",
    "LabelServerHost": "Хост",
    "LabelServerHostHelp": "Жишээ: 192.168.1.100:8096 эсвэл https://myserver.com",
    "Connect": "Холбогдох",
    "Cancel": "Цуцлах",
    "HeaderConnectionFailure": "Холболт алдаа",
    "MessageUnableToConnectToServer": "Сонгосон серверт одоогоор холбогдож чадахгүй байна. Сервер ажиллаж байгааг шалгаад дахин оролдоно уу.",
    "ButtonGotIt": "Ойлголоо"
  },
  {
    "lang": "mr",
    "HeaderConnectToServer": "सर्व्हरशी कनेक्ट करा",
    "LabelServerHost": "सर्व्हर पत्ता",
    "LabelServerHostHelp": "192.168.1.100:8096 किंवा https://myserver.com",
    "Connect": "कनेक्ट करा",
    "Cancel": "रद्द करा",
    "HeaderConnectionFailure": "कनेक्शन अयशस्वी",
    "MessageUnableToConnectToServer": "आम्ही आत्ता निवडलेल्या सर्व्हरशी कनेक्ट करू शकत नाही. कृपया तो चालू असल्याची खात्री करा आणि पुन्हा प्रयत्न करा.",
    "ButtonGotIt": "समजले"
  },
  {
    "lang": "ms",
    "HeaderConnectToServer": "Sambung ke pelayan",
    "LabelServerHost": "Alamat pelayan",
    "LabelServerHostHelp": "192.168.1.100:8096 atau https://myserver.com",
    "Connect": "Sambung",
    "Cancel": "Batalkan",
    "HeaderConnectionFailure": "Sambungan gagal",
    "MessageUnableToConnectToServer": "Kami tidak dapat menyambung ke pelayan yang dipilih sekarang. Sila pastikan ia sedang berjalan dan cuba lagi.",
    "ButtonGotIt": "Terima"
  },
  {
    "lang": "mt",
    "HeaderConnectToServer": "Qabbad mas-server",
    "LabelServerHost": "Indirizz tas-server",
    "LabelServerHostHelp": "192.168.1.100:8096 jew https://myserver.com",
    "Connect": "Qabbad",
    "Cancel": "Ikkanċella",
    "HeaderConnectionFailure": "Il-konnessjoni falliet",
    "MessageUnableToConnectToServer": "Ma nistgħux nikkonnettjaw mas-server magħżul bħalissa. Jekk jogħġbok kun żgur li qed jaħdem u erġa’ pprova.",
    "ButtonGotIt": "Fhimt"
  },
  {
    "lang": "my",
    "HeaderConnectToServer": "ဆာဗာသို့ ချိတ်ဆက်ရန်",
    "LabelServerHost": "ဆာဗာလိပ်စာ",
    "LabelServerHostHelp": "192.168.1.100:8096 သို့မဟုတ် https://myserver.com",
    "Connect": "ချိတ်ဆက်ပါ",
    "Cancel": "ငြင်းပယ်သည်",
    "HeaderConnectionFailure": "ချိတ်ဆက်မှု မအောင်မြင်ပါ",
    "MessageUnableToConnectToServer": "လက်ရှိ ရွေးချယ်ထားသော ဆာဗာသို့ ချိတ်ဆက်၍ မရပါ။ ၎င်း လည်ပတ်နေကြောင်း စစ်ဆေးပြီး ထပ်မံကြိုးစားပါ။",
    "ButtonGotIt": "ရပြီ"
  },
  {
    "lang": "nb",
    "HeaderConnectToServer": "Koble til server",
    "LabelServerHost": "Vertsnavn",
    "LabelServerHostHelp": "192.168.1.100:8096 eller https://minserver.no",
    "Connect": "Koble til",
    "Cancel": "Avbryt",
    "HeaderConnectionFailure": "Tilkobling feilet",
    "MessageUnableToConnectToServer": "Vi klarte ikke å koble til den valgte serveren akkurat nå. Vennligst sørg for at den kjører og prøv på nytt.",
    "ButtonGotIt": "Skjønner"
  },
  {
    "lang": "ne",
    "HeaderConnectToServer": "सर्भरमा जडान गर्नुहोस्",
    "LabelServerHost": "सर्भर ठेगाना",
    "LabelServerHostHelp": "192.168.1.100:8096 वा https://myserver.com",
    "Connect": "जडान गर्नुहोस्",
    "Cancel": "रद्द गर्नुहोस्",
    "HeaderConnectionFailure": "जडान असफल भयो",
    "MessageUnableToConnectToServer": "हामी अहिले चयन गरिएको सर्भरमा जडान गर्न सक्दैनौँ। कृपया यो चलिरहेको छ भनेर सुनिश्चित गरी फेरि प्रयास गर्नुहोस्।",
    "ButtonGotIt": "बुझेँ"
  },
  {
    "lang": "nl",
    "HeaderConnectToServer": "Verbinden met server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 of https://mijnserver.nl",
    "Connect": "Verbinden",
    "Cancel": "Annuleren",
    "HeaderConnectionFailure": "Verbindingsfout",
    "MessageUnableToConnectToServer": "Het is momenteel niet mogelijk met de geselecteerde server te verbinden. Controleer of deze draait en probeer het opnieuw.",
    "ButtonGotIt": "Begrepen"
  },
  {
    "lang": "nn",
    "HeaderConnectToServer": "Kople til tenar",
    "LabelServerHost": "Vert",
    "LabelServerHostHelp": "192.168.1.100:8096 eller https://min-servar.no",
    "Connect": "Kople til",
    "Cancel": "Avbryt",
    "HeaderConnectionFailure": "Tilkoplingsfeil",
    "MessageUnableToConnectToServer": "Vi kan ikkje kople til den valde servaren no. Sjekk at han køyrer og prøv på nytt.",
    "ButtonGotIt": "Skjønner"
  },
  {
    "lang": "pa",
    "HeaderConnectToServer": "ਸਰਵਰ ਨਾਲ ਕਨੈਕਟ ਕਰੋ",
    "LabelServerHost": "ਸਰਵਰ ਪਤਾ",
    "LabelServerHostHelp": "192.168.1.100:8096 ਜਾਂ https://myserver.com",
    "Connect": "ਕਨੈਕਟ ਕਰੋ",
    "Cancel": "ਰੱਦ ਕਰੋ",
    "HeaderConnectionFailure": "ਕਨੈਕਸ਼ਨ ਅਸਫਲ",
    "MessageUnableToConnectToServer": "ਅਸੀਂ ਇਸ ਵੇਲੇ ਚੁਣੇ ਸਰਵਰ ਨਾਲ ਕਨੈਕਟ ਨਹੀਂ ਕਰ ਸਕਦੇ। ਕਿਰਪਾ ਕਰਕੇ ਯਕੀਨੀ ਬਣਾਓ ਕਿ ਇਹ ਚੱਲ ਰਿਹਾ ਹੈ ਅਤੇ ਮੁੜ ਕੋਸ਼ਿਸ਼ ਕਰੋ।",
    "ButtonGotIt": "ਸਮਝ ਆ ਗਿਆ"
  },
  {
    "lang": "pl",
    "HeaderConnectToServer": "Podłącz do serwera",
    "LabelServerHost": "Serwer",
    "LabelServerHostHelp": "192.168.1.100:8096 lub https://mojserwer.pl",
    "Connect": "Połącz",
    "Cancel": "Anuluj",
    "HeaderConnectionFailure": "Niepowodzenie połączenia",
    "MessageUnableToConnectToServer": "Połączenie z wybranym serwerem jest teraz niemożliwe. Upewnij się, że jest uruchomiony i spróbuj ponownie.",
    "ButtonGotIt": "Rozumiem"
  },
  {
    "lang": "pr",
    "HeaderConnectToServer": "Connect to server",
    "LabelServerHost": "Server address",
    "LabelServerHostHelp": "192.168.1.100:8096 or https://myserver.com",
    "Connect": "Connect",
    "Cancel": "Change yer Mind",
    "HeaderConnectionFailure": "Connection failure",
    "MessageUnableToConnectToServer": "We cannot connect to the selected server right now. Make sure it is running and try again.",
    "ButtonGotIt": "Aye-Aye"
  },
  {
    "lang": "pt-br",
    "HeaderConnectToServer": "Conectar ao Servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://meuservidor.com",
    "Connect": "Conectar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Falha na Conexão",
    "MessageUnableToConnectToServer": "Não foi possível conectar ao servidor selecionado. Por favor, verifique se está sendo executado e tente novamente.",
    "ButtonGotIt": "Feito"
  },
  {
    "lang": "pt-pt",
    "HeaderConnectToServer": "Ligar ao servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://omeudominio.com",
    "Connect": "Ligar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Falha de ligação",
    "MessageUnableToConnectToServer": "Não foi possível ligar ao servidor. Verifica se o servidor está em execução e tenta novamente.",
    "ButtonGotIt": "Entendido"
  },
  {
    "lang": "pt",
    "HeaderConnectToServer": "Ligar ao Servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://omeudominio.com",
    "Connect": "Ligar",
    "Cancel": "Cancelar",
    "HeaderConnectionFailure": "Falha de Ligação",
    "MessageUnableToConnectToServer": "Não foi possível estabelecer ligação ao servidor. Por favor, certifique-se que o servidor está a correr e tente de novo.",
    "ButtonGotIt": "Entendido"
  },
  {
    "lang": "ro",
    "HeaderConnectToServer": "Conectați-vă la server",
    "LabelServerHost": "Gazdă",
    "LabelServerHostHelp": "192.168.1.100:8096 sau https://myserver.com",
    "Connect": "Conectare",
    "Cancel": "Anulează",
    "HeaderConnectionFailure": "Conexiune eșuată",
    "MessageUnableToConnectToServer": "Nu putem să ne conectăm la serverul selectat în acest moment. Vă rugăm să vă asigurați că funcționează și încercați din nou.",
    "ButtonGotIt": "Am înțeles"
  },
  {
    "lang": "ru",
    "HeaderConnectToServer": "Соединение с сервером",
    "LabelServerHost": "Узел",
    "LabelServerHostHelp": "192.168.1.100:8096 или https://myserver.com",
    "Connect": "Соединиться",
    "Cancel": "Отменить",
    "HeaderConnectionFailure": "Сбой соединения",
    "MessageUnableToConnectToServer": "Мы не можем подсоединиться к выбранному серверу в данный момент. Убедитесь, что он запущен и повторите попытку.",
    "ButtonGotIt": "Понятно"
  },
  {
    "lang": "si",
    "HeaderConnectToServer": "සේවාදායකයට සම්බන්ධ වන්න",
    "LabelServerHost": "සේවාදායක ලිපිනය",
    "LabelServerHostHelp": "192.168.1.100:8096 හෝ https://myserver.com",
    "Connect": "සම්බන්ධ වන්න",
    "Cancel": "අවලංගු කරන්න",
    "HeaderConnectionFailure": "සම්බන්ධතාව අසාර්ථකයි",
    "MessageUnableToConnectToServer": "තෝරාගත් සේවාදායකයට දැන් සම්බන්ධ විය නොහැක. එය ක්‍රියාත්මකදැයි තහවුරු කර නැවත උත්සාහ කරන්න.",
    "ButtonGotIt": "තේරුණා"
  },
  {
    "lang": "sk",
    "HeaderConnectToServer": "Pripojiť sa k serveru",
    "LabelServerHost": "Hosť",
    "LabelServerHostHelp": "192.168.1.100:8096 alebo https://mojserver.sk",
    "Connect": "Pripojiť",
    "Cancel": "Zrušiť",
    "HeaderConnectionFailure": "Pripojenie zlyhalo",
    "MessageUnableToConnectToServer": "Nie sme schopný sa aktuálne pripojiť k vybranému serveru. Prosím, uistite sa, že je spustený a skúste to znovu.",
    "ButtonGotIt": "Rozumiem"
  },
  {
    "lang": "sl-si",
    "HeaderConnectToServer": "Poveži s strežnikom",
    "LabelServerHost": "Naslov strežnika",
    "LabelServerHostHelp": "192.168.1.100:8096 ali https://myserver.com",
    "Connect": "Poveži",
    "Cancel": "Prekliči",
    "HeaderConnectionFailure": "Napaka povezave",
    "MessageUnableToConnectToServer": "Povezava s strežnikom trenutno ni mogoča. Preverite, da je strežnik zagnan in poskusite ponovno.",
    "ButtonGotIt": "Razumem"
  },
  {
    "lang": "so",
    "HeaderConnectToServer": "Ku xidh server",
    "LabelServerHost": "Cinwaanka server-ka",
    "LabelServerHostHelp": "192.168.1.100:8096 ama https://myserver.com",
    "Connect": "Ku xidh",
    "Cancel": "Jooji",
    "HeaderConnectionFailure": "Xidhiidhku wuu fashilmay",
    "MessageUnableToConnectToServer": "Hadda kuma xirmi karno server-ka la doortay. Fadlan hubi inuu shaqaynayo ka dibna isku day mar kale.",
    "ButtonGotIt": "Waan fahmay"
  },
  {
    "lang": "sq",
    "HeaderConnectToServer": "Lidhuni me serverin",
    "LabelServerHost": "Adresa e serverit",
    "LabelServerHostHelp": "192.168.1.100:8096 ose https://myserver.com",
    "Connect": "Lidhu",
    "Cancel": "Anulo",
    "HeaderConnectionFailure": "Dështim në lidhje",
    "MessageUnableToConnectToServer": "Nuk mund të lidhemi me serverin e zgjedhur tani. Sigurohuni që po funksionon dhe provoni përsëri.",
    "ButtonGotIt": "Kuptova"
  },
  {
    "lang": "sr",
    "HeaderConnectToServer": "Повежи се са сервером",
    "LabelServerHost": "Домаћин",
    "LabelServerHostHelp": "192.168.1.100:8096 или https://myserver.com",
    "Connect": "Повежи",
    "Cancel": "Откажи",
    "HeaderConnectionFailure": "Спајање неуспешно",
    "MessageUnableToConnectToServer": "Тренутно нисмо у могућности да се повежемо са изабраним сервером. Уверите се да је покренут и покушајте поново.",
    "ButtonGotIt": "Разумем"
  },
  {
    "lang": "sv",
    "HeaderConnectToServer": "Anslut till server",
    "LabelServerHost": "Värd",
    "LabelServerHostHelp": "192.168.1.100:8096 eller https://min.server.com",
    "Connect": "Anslut",
    "Cancel": "Avbryt",
    "HeaderConnectionFailure": "Misslyckad anslutning",
    "MessageUnableToConnectToServer": "Vi kunde inte upprätta en anslutning till vald server just nu. Försäkra dig om att den är påslagen och försök igen.",
    "ButtonGotIt": "Ok"
  },
  {
    "lang": "ta",
    "HeaderConnectToServer": "சேவையகத்துடன் இணைக்கவும்",
    "LabelServerHost": "தொகுப்பாளர்",
    "LabelServerHostHelp": "192.168.1.100:8096 or https://myserver.com",
    "Connect": "இணைக்கவும்",
    "Cancel": "ரத்துசெய்",
    "HeaderConnectionFailure": "இணைப்பு தோல்வி",
    "MessageUnableToConnectToServer": "தேர்ந்தெடுக்கப்பட்ட சேவையகத்துடன் இப்போது எங்களால் இணைக்க முடியவில்லை. இது இயங்குவதை உறுதிசெய்து மீண்டும் முயற்சிக்கவும்.",
    "ButtonGotIt": "அறிந்துகொண்டேன்"
  },
  {
    "lang": "te",
    "HeaderConnectToServer": "సర్వర్‌కి కనెక్ట్ చేయండి",
    "LabelServerHost": "హోస్ట్",
    "LabelServerHostHelp": "192.168.1.100:8096 లేదా https://myserver.com",
    "Connect": "కనెక్ట్ చేయండి",
    "Cancel": "రద్దు చేయండి",
    "HeaderConnectionFailure": "కనెక్షన్ వైఫల్యం",
    "MessageUnableToConnectToServer": "మేము ప్రస్తుతం ఎంచుకున్న సర్వర్‌కు కనెక్ట్ చేయలేకపోయాము. దయచేసి ఇది నడుస్తున్నట్లు నిర్ధారించుకోండి మరియు మళ్లీ ప్రయత్నించండి.",
    "ButtonGotIt": "దొరికింది"
  },
  {
    "lang": "th",
    "HeaderConnectToServer": "เชื่อมต่อเซิฟเวอร์",
    "LabelServerHost": "ที่อยู่เซิร์ฟเวอร์",
    "LabelServerHostHelp": "192.168.1.100:8096 หรือ https://myserver.com",
    "Connect": "เชื่อมต่อ",
    "Cancel": "ยกเลิก",
    "HeaderConnectionFailure": "การเชื่อมต่อล้มเหลว",
    "MessageUnableToConnectToServer": "เราไม่สามารถเชื่อมต่อกับเซิร์ฟเวอร์ที่เลือกได้ในขณะนี้ โปรดตรวจสอบว่าเซิร์ฟเวอร์กำลังทำงานอยู่แล้วลองอีกครั้ง",
    "ButtonGotIt": "ตกลง"
  },
  {
    "lang": "tr",
    "HeaderConnectToServer": "Sunucuya Bağlan",
    "LabelServerHost": "Ana Bilgisayar",
    "LabelServerHostHelp": "192.168.1.100:8096 veya https://sunucum.com",
    "Connect": "Bağlan",
    "Cancel": "İptal",
    "HeaderConnectionFailure": "Bağlantı Hatası",
    "MessageUnableToConnectToServer": "Seçilen sunucuya şu anda bağlanamıyoruz. Lütfen sunucunun çalıştığından emin olun ve tekrar deneyin.",
    "ButtonGotIt": "Anlaşıldı"
  },
  {
    "lang": "ug",
    "HeaderConnectToServer": "مۇلازىمېتىرغا ئۇلىنىش",
    "LabelServerHost": "مۇلازىمېتىر ئادرېسى",
    "LabelServerHostHelp": "192.168.1.100:8096 ياكى https://myserver.com",
    "Connect": "ئۇلان",
    "Cancel": "ۋاز كەچ",
    "HeaderConnectionFailure": "ئۇلىنىش مەغلۇپ بولدى",
    "MessageUnableToConnectToServer": "ھازىر تاللانغان مۇلازىمېتىرغا ئۇلىنالمايمىز. ئۇنىڭ ئىشلەۋاتقانلىقىنى تەكشۈرۈپ قايتا سىناڭ.",
    "ButtonGotIt": "چۈشەندىم"
  },
  {
    "lang": "uk",
    "HeaderConnectToServer": "Підключення до сервера",
    "LabelServerHost": "Хост",
    "LabelServerHostHelp": "192.168.1.100:8096 або https://myserver.com",
    "Connect": "Підключитись",
    "Cancel": "Скасувати",
    "HeaderConnectionFailure": "Помилка підключення",
    "MessageUnableToConnectToServer": "Наразі неможливо підключитися до обраного сервера. Будь ласка, переконайтеся, що він запущений і спробуйте ще раз.",
    "ButtonGotIt": "Зрозуміло"
  },
  {
    "lang": "ur_PK",
    "HeaderConnectToServer": "سرور سے جڑیں",
    "LabelServerHost": "میزبان",
    "LabelServerHostHelp": "192.168.1.100:8096 یا https://myserver.com",
    "Connect": "جڑیں",
    "Cancel": "منسوخ کریں",
    "HeaderConnectionFailure": "کنکشن کی ناکامی",
    "MessageUnableToConnectToServer": "ہم ابھی منتخب سرور سے رابطہ قائم کرنے سے قاصر ہیں۔ براہ کرم یقینی بنائیں کہ یہ چل رہا ہے اور دوبارہ کوشش کریں۔",
    "ButtonGotIt": "یہ مل گیا"
  },
  {
    "lang": "uz",
    "HeaderConnectToServer": "Serverga ulanish",
    "LabelServerHost": "Server manzili",
    "LabelServerHostHelp": "192.168.1.100:8096 yoki https://myserver.com",
    "Connect": "Ulanish",
    "Cancel": "Bekor qilish",
    "HeaderConnectionFailure": "Ulanish muvaffaqiyatsiz tugadi",
    "MessageUnableToConnectToServer": "Hozir tanlangan serverga ulana olmayapmiz. Uning ishlayotganini tekshiring va qayta urinib ko‘ring.",
    "ButtonGotIt": "Tushunarli"
  },
  {
    "lang": "vi",
    "HeaderConnectToServer": "Kết Nối Đến Máy Chủ",
    "LabelServerHost": "Máy chủ",
    "LabelServerHostHelp": "192.168.1.100:8096 hoặc https://myserver.com",
    "Connect": "Kết nối",
    "Cancel": "Hủy bỏ",
    "HeaderConnectionFailure": "Kế Nối Thất Bại",
    "MessageUnableToConnectToServer": "Chúng tôi không thể kết nối với máy chủ đã chọn ngay bây giờ. Hãy đảm bảo rằng nó đang chạy và thử lại.",
    "ButtonGotIt": "Hiểu rồi"
  },
  {
    "lang": "zh-cn",
    "HeaderConnectToServer": "连接到服务器",
    "LabelServerHost": "主机",
    "LabelServerHostHelp": "192.168.1.100:8096 或 https://myserver.com",
    "Connect": "连接",
    "Cancel": "取消",
    "HeaderConnectionFailure": "连接失败",
    "MessageUnableToConnectToServer": "现在无法连接所选择的服务器，请确保该服务器目前正在运行。",
    "ButtonGotIt": "确定"
  },
  {
    "lang": "zh-hk",
    "HeaderConnectToServer": "連線去伺服器",
    "LabelServerHost": "主機",
    "LabelServerHostHelp": "例如 192.168.1.100:8096 或者 https://myserver.com",
    "Connect": "連線",
    "Cancel": "取消",
    "HeaderConnectionFailure": "連線失敗",
    "MessageUnableToConnectToServer": "目前連唔到去所選嘅伺服器。唔該確保伺服器行緊，然後再試下。",
    "ButtonGotIt": "了解"
  },
  {
    "lang": "zh-tw",
    "HeaderConnectToServer": "連接至伺服器",
    "LabelServerHost": "主機",
    "LabelServerHostHelp": "192.168.1.100:8096 或是 https://myserver.com",
    "Connect": "連線",
    "Cancel": "取消",
    "HeaderConnectionFailure": "連接失敗",
    "MessageUnableToConnectToServer": "無法連上所選的伺服器，請確保伺服器正在運作中。",
    "ButtonGotIt": "我知道了"
  },
  {
    "lang": "zu",
    "HeaderConnectToServer": "Xhuma kuseva",
    "LabelServerHost": "Ikheli leseva",
    "LabelServerHostHelp": "192.168.1.100:8096 noma https://myserver.com",
    "Connect": "Xhuma",
    "Cancel": "Khansela",
    "HeaderConnectionFailure": "Ukuxhumeka kwehlulekile",
    "MessageUnableToConnectToServer": "Asikwazi ukuxhuma kuseva ekhethiwe njengamanje. Sicela uqinisekise ukuthi iyasebenza bese uzama futhi.",
    "ButtonGotIt": "Ngizwile"
  }
]
;

const fallbackLanguage = 'en-us';

function getDefaultLanguage() {
  if (navigator.language) {
      return navigator.language;
  }
  if (navigator.userLanguage) {
      return navigator.userLanguage;
  }
  if (navigator.languages && navigator.languages.length) {
      return navigator.languages[0];
  }

  return fallbackLanguage;
}

function normalizeLanguage(lang) {
  const normalized = lang.toLowerCase().replace('_', '-');
  if (normalized === 'zh-hans' || normalized === 'zh-sg') {
      return 'zh-cn';
  }
  if (normalized === 'zh-hant' || normalized === 'zh-mo') {
      return 'zh-tw';
  }
  return normalized;
}

let language = normalizeLanguage(getDefaultLanguage());

if (!languages.find(l => l.lang === language)) {
  language = language.split('-')[0];
}

if (!languages.find(l => l.lang === language)) {
  language = fallbackLanguage;
}

const languageStrings = languages.find(l => l.lang === language);
const fallbackStrings = languages.find(l => l.lang === fallbackLanguage);

const titleText = languageStrings.LabelServerHost || fallbackStrings.LabelServerHost || 'Server Address';
const connectText = languageStrings.Connect || fallbackStrings.Connect;
const cancelText = languageStrings.Cancel || fallbackStrings.Cancel || 'Cancel';

const headerConnectionFailureText = languageStrings.HeaderConnectionFailure || fallbackStrings.HeaderConnectionFailure;
const messageUnableToConnectToServerText = languageStrings.MessageUnableToConnectToServer || fallbackStrings.MessageUnableToConnectToServer;
const buttonGotItText = languageStrings.ButtonGotIt || fallbackStrings.ButtonGotIt;

document.getElementById('title').innerText = titleText;
document.getElementById('title').setAttribute('data-original-text', titleText);
document.getElementById('address').placeholder = languageStrings.LabelServerHostHelp || fallbackStrings.LabelServerHostHelp;
document.getElementById('connect-button').innerText = connectText;
document.getElementById('connect-button').setAttribute('data-original-text', connectText);
window.cancelButtonText = cancelText;
