// Translations from jellyfin-web/src/strings/
// Generated with:
// jq -n '[inputs | { lang: input_filename | split(".")[0], HeaderConnectToServer: .HeaderConnectToServer, LabelServerHost: .LabelServerHost, LabelServerHostHelp: .LabelServerHostHelp, Connect: .Connect, HeaderConnectionFailure: .HeaderConnectionFailure, MessageUnableToConnectToServer: .MessageUnableToConnectToServer, ButtonGotIt: .ButtonGotIt }]' *.json

const languages = [
  {
    "lang": "af",
    "HeaderConnectToServer": "Konnekteer aan Bediener",
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Konnekteer",
    "HeaderConnectionFailure": "Konneksie Fout",
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Het Dit So"
  },
  {
    "lang": "ar",
    "HeaderConnectToServer": "اتصل إلى الخادم",
    "LabelServerHost": "المضيف",
    "LabelServerHostHelp": "192.168.1.100:8096 أو https://myserver.com",
    "Connect": "إتصال",
    "HeaderConnectionFailure": "فشل في الاتصال",
    "MessageUnableToConnectToServer": "لم نستطع الاتصال إلى الخادم المختار في الوقت الحالي. الرجاء التأكد من أنه يعمل ثم المحاولة مرة أخرى.",
    "ButtonGotIt": "حسنا"
  },
  {
    "lang": "as",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "be-by",
    "HeaderConnectToServer": "Падлучыцца да сервера",
    "LabelServerHost": "Вядучы",
    "LabelServerHostHelp": "192.168.1.100:8096 або https://myserver.com",
    "Connect": "Падлучыцца",
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
    "HeaderConnectionFailure": "Проблем при свързване",
    "MessageUnableToConnectToServer": "В момента не можем да се свържем с избрания сървър. Моля, уверете се, че работи и опитайте отново.",
    "ButtonGotIt": "Добре"
  },
  {
    "lang": "bn",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "bn_BD",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "কানেক্ট",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "বুঝেছি"
  },
  {
    "lang": "ca",
    "HeaderConnectToServer": "Connectar al servidor",
    "LabelServerHost": "Amfitrió",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://myserver.com",
    "Connect": "Connecta",
    "HeaderConnectionFailure": "Error de connexió",
    "MessageUnableToConnectToServer": "No es pot connectar amb el servidor seleccionat en aquest moment. Assegureu-vos que està funcionant i torni a intentar-ho.",
    "ButtonGotIt": "Entesos"
  },
  {
    "lang": "ch",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "cs",
    "HeaderConnectToServer": "Připojit k serveru",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 nebo https://mujserver.cz",
    "Connect": "Připojit",
    "HeaderConnectionFailure": "Připojení selhalo",
    "MessageUnableToConnectToServer": "Nejsme schopni se připojit k vybranému serveru právě teď. Prosím, ujistěte se, že je spuštěn a zkuste to znovu.",
    "ButtonGotIt": "Rozumím"
  },
  {
    "lang": "cy",
    "HeaderConnectToServer": null,
    "LabelServerHost": "Lletywr",
    "LabelServerHostHelp": null,
    "Connect": "Cysylltu",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Dyna Fe"
  },
  {
    "lang": "da",
    "HeaderConnectToServer": "Forbind til server",
    "LabelServerHost": "Vært",
    "LabelServerHostHelp": "F. eks: 192.168.1.100:8096 eller https://myserver.com",
    "Connect": "Forbind",
    "HeaderConnectionFailure": "Forbindelsesfejl",
    "MessageUnableToConnectToServer": "Vi kan ikke forbinde til den valgte server på nuværende tidspunkt. Sikrer dig venligst at serveren kører og prøv igen.",
    "ButtonGotIt": "Forstået"
  },
  {
    "lang": "de",
    "HeaderConnectToServer": "Mit Server verbinden",
    "LabelServerHost": "Adresse",
    "LabelServerHostHelp": "192.168.1.100:8096 oder https://myserver.com",
    "Connect": "Verbinden",
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
    "HeaderConnectionFailure": "Αποτυχία σύνδεσης",
    "MessageUnableToConnectToServer": "Δεν είναι δυνατή η σύνδεση με τον επιλεγμένο διακομιστή αυτή τη στιγμή. Βεβαιωθείτε ότι εκτελείται και προσπαθήστε ξανά.",
    "ButtonGotIt": "Το κατάλαβα"
  },
  {
    "lang": "en-gb",
    "HeaderConnectToServer": "Connect to Server",
    "LabelServerHost": "Server Address",
    "LabelServerHostHelp": "192.168.1.100:8096 or https://myserver.com",
    "Connect": "Connect",
    "HeaderConnectionFailure": "Connection Failure",
    "MessageUnableToConnectToServer": "We're unable to connect to the selected server right now. Please ensure it is running and try again.",
    "ButtonGotIt": "Got It"
  },
  {
    "lang": "en-us",
    "HeaderConnectToServer": "Connect to Server",
    "LabelServerHost": "Server Address",
    "LabelServerHostHelp": "192.168.1.100:8096 or https://myserver.com",
    "Connect": "Connect",
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
    "HeaderConnectionFailure": "Conexión fallida",
    "MessageUnableToConnectToServer": "No podemos conectarnos al servidor seleccionado en este momento. Asegúrese de que se esté ejecutando e intente nuevamente.",
    "ButtonGotIt": "Lo entendí"
  },
  {
    "lang": "es-mx",
    "HeaderConnectToServer": "Conectarse al servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://miservidor.com",
    "Connect": "Conectar",
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
    "HeaderConnectionFailure": "Falla de conexión",
    "MessageUnableToConnectToServer": "No podemos conectarnos al servidor seleccionado en este momento. Por favor, asegúrate de que está funcionando e inténtalo de nuevo.",
    "ButtonGotIt": "Hecho"
  },
  {
    "lang": "es_DO",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "et",
    "HeaderConnectToServer": "Ühendu serveriga",
    "LabelServerHost": "Peremeesmasin",
    "LabelServerHostHelp": "192.168.1.100:8096 või https://myserver.com",
    "Connect": "Ühenda",
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
    "HeaderConnectionFailure": "عدم اتصال",
    "MessageUnableToConnectToServer": "",
    "ButtonGotIt": "متوجه شدم"
  },
  {
    "lang": "fi",
    "HeaderConnectToServer": "Yhdistä palvelimeen",
    "LabelServerHost": "Isäntä",
    "LabelServerHostHelp": "192.168.1.100:8096 tai https://myserver.com",
    "Connect": "Yhdistä",
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
    "HeaderConnectionFailure": "Nag-fail ang koneksyon",
    "MessageUnableToConnectToServer": "Hindi kami makakonekta sa napiling server sa ngayon. Pakitiyak na ito ay tumatakbo at subukang muli.",
    "ButtonGotIt": "Nakuha ko"
  },
  {
    "lang": "fo",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "fr-ca",
    "HeaderConnectToServer": "Connexion au serveur",
    "LabelServerHost": "Hôte",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://monserveur.com",
    "Connect": "Connexion",
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
    "HeaderConnectionFailure": "Échec de connexion",
    "MessageUnableToConnectToServer": "Nous sommes dans l'impossibilité de nous connecter au serveur sélectionné. Veuillez vérifier qu'il est opérationnel et réessayez.",
    "ButtonGotIt": "Compris"
  },
  {
    "lang": "ga",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "gl",
    "HeaderConnectToServer": "Conectar ao Servidor",
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Conectar",
    "HeaderConnectionFailure": "Fallo de Conexión",
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Entendo"
  },
  {
    "lang": "gsw",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "gu",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "he",
    "HeaderConnectToServer": "התחבר לשרת",
    "LabelServerHost": "מארח",
    "LabelServerHostHelp": "192.168.1.100:8096 או https://myserver.com",
    "Connect": "התחבר",
    "HeaderConnectionFailure": "כשל בחיבור",
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "הבנתי"
  },
  {
    "lang": "hi-in",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "समझ गया"
  },
  {
    "lang": "hr",
    "HeaderConnectToServer": "Spoji se na Server",
    "LabelServerHost": "Domaćin",
    "LabelServerHostHelp": "192.168.1.100:8096 ili https://myserver.com",
    "Connect": "Povezati",
    "HeaderConnectionFailure": "Neuspjelo spajanje",
    "MessageUnableToConnectToServer": "Nismo u mogućnosti spojiti se na odabrani poslužitelj. Provjerite dali je pokrenut i pokušajte ponovno.",
    "ButtonGotIt": "Shvaćam"
  },
  {
    "lang": "hu",
    "HeaderConnectToServer": "Kapcsolódás a Szerverhez",
    "LabelServerHost": "Kiszolgáló",
    "LabelServerHostHelp": "192.168.1.100:8096 vagy https://myserver.com",
    "Connect": "Kapcsolódás",
    "HeaderConnectionFailure": "Kapcsolathiba",
    "MessageUnableToConnectToServer": "Jelenleg nem tudunk csatlakozni a kiválasztott szerverhez. Győződj meg róla, hogy fut és próbáld meg újra.",
    "ButtonGotIt": "Értettem"
  },
  {
    "lang": "hy",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": "192.168.1.100:8096 կամ https://myserver.com",
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "id",
    "HeaderConnectToServer": "Sambungkan ke server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 atau https://myserver.com",
    "Connect": "Sambung",
    "HeaderConnectionFailure": "Koneksi Bermasalah",
    "MessageUnableToConnectToServer": "Kami tidak dapat terhubung ke server yang dipilih sekarang. Harap pastikan itu berjalan dan coba lagi.",
    "ButtonGotIt": "Paham"
  },
  {
    "lang": "is-is",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Tengjast",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Skilið"
  },
  {
    "lang": "it",
    "HeaderConnectToServer": "Connettersi al Server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 o https://myserver.com",
    "Connect": "Connetti",
    "HeaderConnectionFailure": "Errore di connessione",
    "MessageUnableToConnectToServer": "Non siamo in grado di connettersi al server selezionato al momento. Per favore assicurati che sia in esecuzione e riprova.",
    "ButtonGotIt": "Ho capito"
  },
  {
    "lang": "ja",
    "HeaderConnectToServer": "サーバーに接続",
    "LabelServerHost": "ホスト",
    "LabelServerHostHelp": "192.168.1.100:8096 又は https://myserver.com",
    "Connect": "接続",
    "HeaderConnectionFailure": "接続失敗",
    "MessageUnableToConnectToServer": "現在、選択されたサーバーへの接続ができません。稼働していることを確認しもう一度やり直してください。",
    "ButtonGotIt": "了解"
  },
  {
    "lang": "jbo",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "je'e"
  },
  {
    "lang": "ka",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "გასაგებია"
  },
  {
    "lang": "kab",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "kk",
    "HeaderConnectToServer": "Serverge qosylu",
    "LabelServerHost": "Tüiın",
    "LabelServerHostHelp": "192.168.1.100:8096 nemese https://myserver.com",
    "Connect": "Qosylu",
    "HeaderConnectionFailure": "Qosylu sätsız",
    "MessageUnableToConnectToServer": "Tañdalğan serverge qosyluymyz däl qazır mümkın emes. Būl ıske qosylğanyna köz jetkızıñız jäne ärekettı keiın qaitalañyz.",
    "ButtonGotIt": "Tüsınıktı"
  },
  {
    "lang": "kn",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "ko",
    "HeaderConnectToServer": "서버 접속",
    "LabelServerHost": "호스트",
    "LabelServerHostHelp": "192.168.1.100:8096 또는 https://myserver.com",
    "Connect": "접속",
    "HeaderConnectionFailure": "연결 실패",
    "MessageUnableToConnectToServer": "선택한 서버에 연결할 수 없습니다. 서버가 실행 중인지 확인후 다시 시도하세요.",
    "ButtonGotIt": "알겠습니다"
  },
  {
    "lang": "kw",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "ky",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "lt-lt",
    "HeaderConnectToServer": "Prisijungti prie Serverio",
    "LabelServerHost": null,
    "LabelServerHostHelp": "192.168.1.100:8096 arba https://manoserveris.lt",
    "Connect": "Prisijungti",
    "HeaderConnectionFailure": "Prisijungimo klaida",
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Supratau"
  },
  {
    "lang": "lv",
    "HeaderConnectToServer": "Pievienoties pie servera",
    "LabelServerHost": "Resursdators",
    "LabelServerHostHelp": "192.168.1.100:8096 vai https://myserver.com",
    "Connect": "Savienot",
    "HeaderConnectionFailure": "Savienojuma kļūda",
    "MessageUnableToConnectToServer": "Mēs pašlaik nevaram sazināties ar izvēlēto serveri. Pārliecinies ka tas strādā, un mēģini vēlreiz.",
    "ButtonGotIt": "Sapratu"
  },
  {
    "lang": "mg",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "mk",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Поврзи",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Потврдувам"
  },
  {
    "lang": "ml",
    "HeaderConnectToServer": "സെർവറിലേക്ക് കണക്റ്റുചെയ്യുക",
    "LabelServerHost": "ഹോസ്റ്റ്",
    "LabelServerHostHelp": "192.168.1.100:8096 അല്ലെങ്കിൽ https://myserver.com",
    "Connect": "ബന്ധിപ്പിക്കുക",
    "HeaderConnectionFailure": "കണക്ഷൻ പരാജയം",
    "MessageUnableToConnectToServer": "തിരഞ്ഞെടുത്ത സെർവറിലേക്ക് ഞങ്ങൾക്ക് ഇപ്പോൾ കണക്റ്റുചെയ്യാൻ കഴിയില്ല. ഇത് പ്രവർത്തിക്കുന്നുവെന്ന് ഉറപ്പാക്കി വീണ്ടും ശ്രമിക്കുക.",
    "ButtonGotIt": "മനസ്സിലായി"
  },
  {
    "lang": "mn",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "mr",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "समजले"
  },
  {
    "lang": "ms",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Sambung",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Terima"
  },
  {
    "lang": "mt",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "my",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "ချိတ်ဆက်ပါ",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "ရပြီ"
  },
  {
    "lang": "nb",
    "HeaderConnectToServer": "Koble til server",
    "LabelServerHost": "Vertsnavn",
    "LabelServerHostHelp": "192.168.1.100:8096 eller https://minserver.no",
    "Connect": "Koble til",
    "HeaderConnectionFailure": "Tilkobling feilet",
    "MessageUnableToConnectToServer": "Vi klarte ikke å koble til den valgte serveren akkurat nå. Vennligst sørg for at den kjører og prøv på nytt.",
    "ButtonGotIt": "Skjønner"
  },
  {
    "lang": "ne",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "nl",
    "HeaderConnectToServer": "Verbinden met server",
    "LabelServerHost": "Host",
    "LabelServerHostHelp": "192.168.1.100:8096 of https://mijnserver.nl",
    "Connect": "Verbinden",
    "HeaderConnectionFailure": "Verbindingsfout",
    "MessageUnableToConnectToServer": "Het is momenteel niet mogelijk met de geselecteerde server te verbinden. Controleer of deze draait en probeer het opnieuw.",
    "ButtonGotIt": "Begrepen"
  },
  {
    "lang": "nn",
    "HeaderConnectToServer": "Kople til tenar",
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Kople til",
    "HeaderConnectionFailure": "Tilkoplingsfeil",
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Skjønner"
  },
  {
    "lang": "pa",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "ਕਨੈਕਟ ਕਰੋ",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "pl",
    "HeaderConnectToServer": "Podłącz do serwera",
    "LabelServerHost": "Serwer",
    "LabelServerHostHelp": "192.168.1.100:8096 lub https://mojserwer.pl",
    "Connect": "Połącz",
    "HeaderConnectionFailure": "Niepowodzenie połączenia",
    "MessageUnableToConnectToServer": "Połączenie z wybranym serwerem jest teraz niemożliwe. Upewnij się, że jest uruchomiony i spróbuj ponownie.",
    "ButtonGotIt": "Rozumiem"
  },
  {
    "lang": "pr",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Aye-Aye"
  },
  {
    "lang": "pt-br",
    "HeaderConnectToServer": "Conectar ao Servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://meuservidor.com",
    "Connect": "Conectar",
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
    "HeaderConnectionFailure": "Falha de ligação",
    "MessageUnableToConnectToServer": "Não foi possível estabelecer ligação ao servidor. Por favor, certifique-se de que o servidor está a correr e tente de novo.",
    "ButtonGotIt": "Entendido"
  },
  {
    "lang": "pt",
    "HeaderConnectToServer": "Ligar ao Servidor",
    "LabelServerHost": "Servidor",
    "LabelServerHostHelp": "192.168.1.100:8096 ou https://omeudominio.com",
    "Connect": "Ligar",
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
    "HeaderConnectionFailure": "Сбой соединения",
    "MessageUnableToConnectToServer": "Мы не можем подсоединиться к выбранному серверу в данный момент. Убедитесь, что он запущен и повторите попытку.",
    "ButtonGotIt": "Понятно"
  },
  {
    "lang": "si",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "sk",
    "HeaderConnectToServer": "Pripojiť sa k serveru",
    "LabelServerHost": "Hosť",
    "LabelServerHostHelp": "192.168.1.100:8096 alebo https://mojserver.sk",
    "Connect": "Pripojiť",
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
    "HeaderConnectionFailure": "Napaka povezave",
    "MessageUnableToConnectToServer": "Povezava s strežnikom trenutno ni mogoča. Preverite, da je strežnik zagnan in poskusite ponovno.",
    "ButtonGotIt": "Razumem"
  },
  {
    "lang": "so",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "sq",
    "HeaderConnectToServer": "Lidhuni me serverin",
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Lidhu",
    "HeaderConnectionFailure": "Dështim në lidhje",
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Kuptova"
  },
  {
    "lang": "sr",
    "HeaderConnectToServer": "Повежи се са сервером",
    "LabelServerHost": "Домаћин",
    "LabelServerHostHelp": "192.168.1.100:8096 или https://myserver.com",
    "Connect": "Повежи",
    "HeaderConnectionFailure": "Спајање неуспешно",
    "MessageUnableToConnectToServer": "Тренутно нисмо у могућности да се повежемо са изабраним сервером. Уверите се да је покренут и покушајте поново.",
    "ButtonGotIt": "У реду"
  },
  {
    "lang": "sv",
    "HeaderConnectToServer": "Anslut till server",
    "LabelServerHost": "Värd",
    "LabelServerHostHelp": "192.168.1.100:8096 eller https://min.server.com",
    "Connect": "Anslut",
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
    "HeaderConnectionFailure": "இணைப்பு தோல்வி",
    "MessageUnableToConnectToServer": "தேர்ந்தெடுக்கப்பட்ட சேவையகத்துடன் இப்போது எங்களால் இணைக்க முடியவில்லை. இது இயங்குவதை உறுதிசெய்து மீண்டும் முயற்சிக்கவும்.",
    "ButtonGotIt": "அறிந்துகொண்டேன்"
  },
  {
    "lang": "te",
    "HeaderConnectToServer": "సర్వర్‌కు కనెక్ట్ అవ్వండి",
    "LabelServerHost": "హోస్ట్",
    "LabelServerHostHelp": "192.168.1.100:8096 లేదా https://myserver.com",
    "Connect": "కనెక్ట్ చేయండి",
    "HeaderConnectionFailure": "కనెక్షన్ వైఫల్యం",
    "MessageUnableToConnectToServer": "మేము ప్రస్తుతం ఎంచుకున్న సర్వర్‌కు కనెక్ట్ చేయలేకపోయాము. దయచేసి ఇది నడుస్తున్నట్లు నిర్ధారించుకోండి మరియు మళ్లీ ప్రయత్నించండి.",
    "ButtonGotIt": "దొరికింది"
  },
  {
    "lang": "th",
    "HeaderConnectToServer": "เชื่อมต่อเซิฟเวอร์",
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "เชื่อมต่อ",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "tr",
    "HeaderConnectToServer": "Sunucuya Bağlan",
    "LabelServerHost": "Ana Bilgisayar",
    "LabelServerHostHelp": "192.168.1.100:8096 veya https://sunucum.com",
    "Connect": "Bağlan",
    "HeaderConnectionFailure": "Bağlantı Hatası",
    "MessageUnableToConnectToServer": "Seçilen sunucuya şu anda bağlanamıyoruz. Lütfen sunucunun çalıştığından emin olun ve tekrar deneyin.",
    "ButtonGotIt": "Anlaşıldı"
  },
  {
    "lang": "ug",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": null,
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
  },
  {
    "lang": "uk",
    "HeaderConnectToServer": "Підключення до сервера",
    "LabelServerHost": "Хост",
    "LabelServerHostHelp": "192.168.1.100:8096 або https://myserver.com",
    "Connect": "Підключитись",
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
    "HeaderConnectionFailure": "کنکشن کی ناکامی",
    "MessageUnableToConnectToServer": "ہم ابھی منتخب سرور سے رابطہ قائم کرنے سے قاصر ہیں۔ براہ کرم یقینی بنائیں کہ یہ چل رہا ہے اور دوبارہ کوشش کریں۔",
    "ButtonGotIt": "یہ مل گیا"
  },
  {
    "lang": "uz",
    "HeaderConnectToServer": "Serverga ulanish",
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Ulanish",
    "HeaderConnectionFailure": "Ulanish muvaffaqiyatsiz tugadi",
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": "Tushunarli"
  },
  {
    "lang": "vi",
    "HeaderConnectToServer": "Kết Nối Đến Máy Chủ",
    "LabelServerHost": "Máy chủ",
    "LabelServerHostHelp": "192.168.1.100:8096 hoặc https://myserver.com",
    "Connect": "Kết nối",
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
    "HeaderConnectionFailure": "连接失败",
    "MessageUnableToConnectToServer": "现在无法连接所选择的服务器，请确保该服务器目前正在运行。",
    "ButtonGotIt": "知道了"
  },
  {
    "lang": "zh-hk",
    "HeaderConnectToServer": "連接至伺服器",
    "LabelServerHost": "主機",
    "LabelServerHostHelp": "192.168.1.100:8096 或是 https://myserver.com",
    "Connect": "連接",
    "HeaderConnectionFailure": "連接失敗",
    "MessageUnableToConnectToServer": "無法連接到所選的伺服器，請先檢查伺服器的運作情況。",
    "ButtonGotIt": "了解"
  },
  {
    "lang": "zh-tw",
    "HeaderConnectToServer": "連接至伺服器",
    "LabelServerHost": "主機",
    "LabelServerHostHelp": "192.168.1.100:8096 或是 https://myserver.com",
    "Connect": "連線",
    "HeaderConnectionFailure": "連接失敗",
    "MessageUnableToConnectToServer": "無法連上所選的伺服器，請確保伺服器正在運作中。",
    "ButtonGotIt": "我知道了"
  },
  {
    "lang": "zu",
    "HeaderConnectToServer": null,
    "LabelServerHost": null,
    "LabelServerHostHelp": null,
    "Connect": "Xhuma",
    "HeaderConnectionFailure": null,
    "MessageUnableToConnectToServer": null,
    "ButtonGotIt": null
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

let language = getDefaultLanguage().toLowerCase();

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

document.getElementById('title').innerText = titleText;
document.getElementById('title').setAttribute('data-original-text', titleText);
document.getElementById('address').placeholder = languageStrings.LabelServerHostHelp || fallbackStrings.LabelServerHostHelp;
document.getElementById('connect-button').innerText = connectText;
document.getElementById('connect-button').setAttribute('data-original-text', connectText);
window.cancelButtonText = 'Cancel';
