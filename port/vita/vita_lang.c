/*
 * The port's own text in Spanish: what the Vita adds to the game's screens (Vita Settings' guide lines, the page
 * counter, the versus menu's Multiplayer choice and what it says while it looks for the other Vita). The game's own
 * text is the disc's: BT4's Spanish release (Team BT4's) has all of it in Spanish, and its voices.
 *
 * Vita_Tr("English") is the Spanish of that text while the game is in Spanish (Vita_Spanish), else the text as it is.
 * A text with %s, %d or %c keeps them, in the same order. The guide's lines hold 94 letters at most (their slots: 192
 * bytes of UTF-16), about 36 to a line; the letters are UTF-8 here, the Spanish discs' lettering has them (á é í ó ú
 * ñ ü ¿ ¡); the page counter's font has no accents.
 *
 * BT3: its Spanish is the European disc's (vita_eu.c): the menus, the HUD, the subtitles and, in Spanish, the options
 * screen too (its Vita art made from it, vita_optart_es.h).
 */
#include <string.h>

#ifdef BT3_BT4
extern int Port_GameProfileEnglish(void);
#endif

int Vita_Spanish(void) {
#ifdef BT3_BT4
    return !Port_GameProfileEnglish();
#else
    /* the options screen's own language while it is up (it is the USA disc's until it is opened again after the
       language changed), else the language chosen (vita_eu.c) */
    extern int Vita_OptionArtLang(void);
    extern int VitaEu_Lang(void);
    int screen = Vita_OptionArtLang();
    return screen >= 0 ? screen == 1 : VitaEu_Lang() == 1;
#endif
}

static const char *const kEs[][2] = {
    /* Vita Settings (src/menu/menu_x_c.c Option_VitaSetGuide) */
    {"ON", "Sí"},
    {"OFF", "No"},
    {"Page %d/%d", "Pag. %d/%d"},
    {"Screen: the camera, the screen's\nshake, its position and the tag\nteam's HUD.",
     "Pantalla: la cámara, la vibración\nde pantalla, su posición y el\nHUD de los equipos."},
    {"Unlock All (%s): every fighter and\nform, stage, song and item, and\n9,999,999 Zenni.",
     "Desbloquear (%s): luchadores,\nformas, escenarios, canciones,\nobjetos y 9.999.999 Zenni."},
    {"Game: Team BT4's Budokai Tenkaichi\n4, or Budokai Tenkaichi 3.",
     "Juego: Budokai Tenkaichi 4 de\nTeam BT4, o Budokai Tenkaichi 3."},
    {"Touch: where L2, R2, L3 and R3\nare touched.", "Táctil: dónde se tocan L2, R2,\nL3 y R3."},
    {"Clashes: %s\nwins beam and rush clashes.", "Choques: %s\ngana los choques de rayos y\nde golpes."},
    {"pressing Square fast", "pulsar Cuadrado rápido"},
    {"turning the left stick", "girar la palanca izquierda"},
    {"2nd Pad: a second controller\nis player 2 (PS TV): %s.", "Control 2: un segundo control es el\njugador 2 (PS TV): %s."},
    {"Mods (ux0:data/bt3/mods): ON.\nIn use: %s.", "Mods (ux0:data/bt3/mods): Sí.\nEn uso: %s."},
    {"Mods: files in ux0:data/bt3/mods\nreplace the disc's: OFF.",
     "Mods: archivos de ux0:data/bt3/mods\nen lugar de los del disco: No."},
    {"%s is %s.\nChoose a button to change it.", "%s: %s.\nElige un botón para cambiarlo."},
    {"the rear touch pad's\n%s quarter", "el cuarto %s\ndel panel táctil trasero"},
    {"upper left", "superior izquierdo"},
    {"upper right", "superior derecho"},
    {"lower left", "inferior izquierdo"},
    {"lower right", "inferior derecho"},
    {"the screen's %s\ncorner", "la esquina %s\nde la pantalla"},
    {"bottom left", "inferior izquierda"},
    {"bottom right", "inferior derecha"},
    {"top left", "superior izquierda"},
    {"top right", "superior derecha"},
    {"SELECT + %c only", "solo SELECT + %c"},
    {"not on the touch panels", "fuera de los paneles táctiles"},
    {"%s on the rear touch pad's\n%s quarter, where a grip\nattachment's %s presses.",
     "%s en el cuarto %s\ndel panel trasero (donde pulsa\nun accesorio de agarre)."},
    {"%s on the screen's %s\ncorner.", "%s en la esquina %s\nde la pantalla."},
    {"%s not on the touch panels\n(SELECT + %c still gives it).", "%s fuera de los paneles táctiles\n(SELECT + %c lo sigue dando)."},
    {"%s not on the touch panels.", "%s fuera de los paneles táctiles."},
    {"Square: press it again and again\nto win beam and rush clashes.",
     "Cuadrado: púlsalo una y otra vez\npara ganar choques de rayos y\nde golpes."},
    {"Stick: turn the left stick round\nto win clashes, as on the PS2.",
     "Palanca: gira la palanca izquierda\npara ganar choques, como en PS2."},
    {"ON: a second controller plays\nas player 2 (Versus: Multiplayer).",
     "Sí: un segundo control juega como\njugador 2 (Versus: Multijugador)."},
    {"OFF: a second controller is not\nused.", "No: no se usa un segundo control."},
    {"Budokai Tenkaichi 4: the game you\nare playing now.", "Budokai Tenkaichi 4: el juego al\nque estás jugando."},
    {"Budokai Tenkaichi 3 could not be\nstarted (see the log).", "No se pudo iniciar Budokai\nTenkaichi 3 (mira el registro)."},
    {"Budokai Tenkaichi 3. Game starts\neach time, until changed back.\nPress again to confirm.",
     "Budokai Tenkaichi 3. Se inicia\nsiempre, hasta que lo cambies.\nPulsa otra vez para confirmar."},
    {"Budokai Tenkaichi 3. Game starts\neach time, until changed back.",
     "Budokai Tenkaichi 3. Se inicia\nsiempre, hasta que lo cambies."},
    {"Done: everything is unlocked. It\nis saved as you leave the options.",
     "Hecho: todo está desbloqueado. Se\nguarda al salir de las opciones."},
    {"Done: your save's own unlocks and\nZenni are back. Saved as you\nleave the options.",
     "Hecho: vuelven los desbloqueos y\nZenni de tu partida. Se guarda\nal salir de las opciones."},
    {"OFF: no longer given to each save.\nWhat is unlocked stays (there was\nno copy of the save from before).",
     "No: ya no se da a cada partida. Lo\ndesbloqueado se queda (no había\ncopia de la partida)."},
    {"Not turned on: the copy of your\nsave could not be made (see the\nlog).",
     "No activado: no se pudo copiar\ntu partida (mira el registro)."},
    {"ON: everything is unlocked, for\neach save loaded.", "Sí: todo desbloqueado en cada\npartida que se cargue."},
    {"Press CROSS again to unlock\neverything (Circle: no).", "Pulsa X otra vez para\ndesbloquearlo todo (Círculo: no)."},
    {"Every fighter and form, stage, song\nand item (Evolution Z, Potara),\n9,999,999 Zenni. CROSS twice.",
     "Todos los luchadores, formas,\nescenarios, canciones, objetos y\n9.999.999 Zenni. X dos veces."},
    {"OFF: your save's own unlocks.", "No: los desbloqueos de tu partida."},
    {"Press CROSS again: the unlocks and\nZenni go back to how they were\n(Circle: no).",
     "Pulsa X otra vez: desbloqueos y\nZenni vuelven a como estaban\n(Círculo: no)."},
    {"Your save's unlocks and Zenni as\nthey were before ON. CROSS twice.",
     "Tus desbloqueos y Zenni como\nestaban antes. X dos veces."},
    {"Stops unlocking each save; what is\nunlocked stays. CROSS twice.",
     "Deja de desbloquear cada partida;\nlo desbloqueado se queda. X dos\nveces."},
    {"ON: files in ux0:data/bt3/mods\nreplace the disc's, from the\nnext start.",
     "Sí: los archivos de ux0:data/bt3/\nmods sustituyen a los del disco,\ndesde el próximo inicio."},
    {"OFF: the disc's files only, from\nthe next start.", "No: solo los archivos del disco,\ndesde el próximo inicio."},
    /* BT3's Vita Settings */
    {"Screen: the camera, the screen's\nshake and its position.", "Pantalla: la cámara, la vibración\nde pantalla y su posición."},
    {"Game: Budokai Tenkaichi 3, or Team\nBT4's Budokai Tenkaichi 4 mod.", "Juego: Budokai Tenkaichi 3, o el\nmod Budokai Tenkaichi 4 de Team BT4."},
    {"Quick Start: no logos, opening film\nor copyright page at the start: %s.",
     "Inicio Rápido: sin logos, vídeo\nde apertura ni página de copyright\nal empezar: %s."},
    {"Quick Save: saving and loading\nwithout the memory card window: %s.",
     "Guardado Rápido: guardar y cargar\nsin la ventana de la tarjeta de\nmemoria: %s."},
    {"Tech Fixes: the community's fixes\nof the fighting: Z Counter %s,\nBombardment %s.",
     "Arreglos: los arreglos de la\ncomunidad al combate: Contador Z\n%s, Bombardeo %s."},
    {"Not turned on: the copy of your\nsave could not be made (see\nux0:data/bt3/log.txt).",
     "No activado: no se pudo copiar tu\npartida (mira ux0:data/bt3/log.txt)."},
    {"Z Counter fix (%s): keeps a timed\nZ Counter the game loses when you\nmove as the attack lands.",
     "Contador Z (%s): mantiene un\nContador Z a tiempo que el juego\npierde si te mueves al golpear."},
    {"Bombardment fix (%s): a chain of\nsmash Ki blasts with dashes is not\nbroken by body contact.",
     "Bombardeo (%s): una cadena de\nráfagas de Ki con embestidas no\nse rompe por el contacto."},
    {"Budokai Tenkaichi 3: the game you\nare playing now.", "Budokai Tenkaichi 3: el juego al\nque estás jugando."},
    {"This package has no Budokai\nTenkaichi 4 program.", "Este paquete no tiene el programa\nde Budokai Tenkaichi 4."},
    {"Not now: copy your BT4 B14 Rev2\ndisc's .iso to ux0:data/bt3/bt4/\nfirst (see the README).",
     "Ahora no: copia antes el .iso de\ntu disco BT4 B14 Rev2 en\nux0:data/bt3/bt4/ (ver el README)."},
    {"Budokai Tenkaichi 4 could not be\nstarted (see ux0:data/bt3/log.txt).",
     "No se pudo iniciar Budokai Tenkaichi\n4 (mira ux0:data/bt3/log.txt)."},
    {"Budokai Tenkaichi 4. Game starts\neach time, until changed back.\nPress again to confirm.",
     "Budokai Tenkaichi 4. Se inicia\nsiempre, hasta que lo cambies.\nPulsa otra vez para confirmar."},
    {"Budokai Tenkaichi 4. Game starts\neach time, until changed back.",
     "Budokai Tenkaichi 4. Se inicia\nsiempre, hasta que lo cambies."},
    {"ON: the game starts at its title\nscreen (no logos, opening film or\ncopyright page).",
     "Sí: el juego empieza en su pantalla\nde título (sin logos, vídeo de\napertura ni copyright)."},
    {"OFF: the logos, the opening film\nand the copyright page, as the\ngame has them.",
     "No: los logos, el vídeo de apertura\ny la página de copyright, como\nlos tiene el juego."},
    {"ON: saving and loading go without\nthe memory card window, unless\nthere is a question to answer.",
     "Sí: guardar y cargar sin la ventana\nde la tarjeta de memoria, salvo\nsi hay una pregunta."},
    {"OFF: the memory card window and\nits pages, as the game has them.", "No: la ventana de la tarjeta de\nmemoria, como la tiene el juego."},
    /* BT3's Language row (Language/Sound > Language: the plates are the disc's, Inglés and Japonés in Spanish) */
    {"English: the menus, the text and\nthe voices in English.", "Inglés: los menús, el texto y\nlas voces en inglés."},
    {"Japanese: the voices in Japanese,\nthe menus and subtitles in\nEnglish.", "Japonés: las voces en japonés,\nlos menús y subtítulos en inglés."},
    {"Espanol: the menus, the HUD and\nthe subtitles in Spanish, from\nthe next screen.",
     "Español: los menús, el HUD y los\nsubtítulos en español, desde la\nsiguiente pantalla."},
    {"Deutsch: the menus, the HUD and\nthe subtitles in German, from\nthe next screen.",
     "Deutsch: los menús, el HUD y los\nsubtítulos en alemán, desde la\nsiguiente pantalla."},
    {"Francais: the menus, the HUD and\nthe subtitles in French, from\nthe next screen.",
     "Français: los menús, el HUD y los\nsubtítulos en francés, desde la\nsiguiente pantalla."},
    {"Italiano: the menus, the HUD and\nthe subtitles in Italian, from\nthe next screen.",
     "Italiano: los menús, el HUD y los\nsubtítulos en italiano, desde la\nsiguiente pantalla."},
    {"The voices in English (the\nEuropean disc has English and\nJapanese ones).",
     "Las voces en inglés (el disco\neuropeo las tiene en inglés y\njaponés)."},
    {"The voices in Japanese.", "Las voces en japonés."},
    /* BT4's Language: its English and its Spanish release */
    {"Language: %s. Team BT4's English\nor Spanish release: text, voices.",
     "Idioma: %s. Versión inglesa o\nespañola de Team BT4: texto, voces."},
    {"%s: the language you are\nplaying in now.", "%s: el idioma en el que\nestás jugando."},
    {"English: Team BT4's English\nrelease, its text and voices.\nThe game starts again.",
     "English: la versión inglesa de\nTeam BT4, texto y voces. El\njuego se reinicia."},
    {"Español: Team BT4's Spanish\nrelease, its text and voices.\nThe game starts again.",
     "Español: la versión española de\nTeam BT4, texto y voces. El\njuego se reinicia."},
    {"Press again to confirm: the\ngame starts again in %s.", "Pulsa otra vez para confirmar:\nel juego se reinicia en %s."},
    {"Not there: copy that release's\n.iso into %s", "No está: copia el .iso de esa\nversión en %s"},
    {"The game could not be started\nagain (see the log).", "No se pudo reiniciar el juego\n(mira el registro)."},
    /* the options screen's guide lines (vita_optart.c) */
    {"Touch buttons, clashes, the\nscreen, the language and more.",
     "Botones táctiles, choques, la\npantalla, el idioma y más."},
    /* the versus menu's Multiplayer (vita_optart.c, vita_net.c) */
    {"Two players on this Vita,\nwith a second controller.", "Dos jugadores en esta Vita,\ncon un segundo control."},
    {"Host a match for a Vita nearby\n(ad hoc: no router needed).", "Crea una partida para una Vita\ncercana (ad hoc: sin router)."},
    {"Join a match that a Vita nearby\nhosts (ad hoc).", "Únete a la partida de una Vita\ncercana (ad hoc)."},
    {"Connecting (ad hoc) ...", "Conectando (ad hoc) ..."},
    {"The ad hoc connection was\ncancelled.", "Se canceló la conexión\nad hoc."},
    {"The ad hoc connection was\nnot made.", "No se hizo la conexión\nad hoc."},
    {"The ad hoc connection could\nnot be used.", "No se pudo usar la conexión\nad hoc."},
    {"Waiting for a player to join ...\n(Triangle: cancel)", "Esperando a un jugador ...\n(Triángulo: cancelar)"},
    {"Looking for a match nearby ...\n(Triangle: cancel)", "Buscando una partida cerca ...\n(Triángulo: cancelar)"},
    {"%.23s is joining ...", "%.23s se está uniendo ..."},
    {"%.23s joined.\nThe match begins ...", "%.23s se unió.\nEmpieza la partida ..."},
    {"%.23s did not answer.\nLooking again ...", "%.23s no respondió.\nBuscando otra vez ..."},
    {"%.23s has another version\nof the game: cannot join.", "%.23s tiene otra versión\ndel juego: no puede unirse."},
    {"%.23s has other disc files:\ncannot join.", "%.23s tiene otros archivos\ndel disco: no puede unirse."},
    {"Joining %.23s ...", "Uniéndose a %.23s ..."},
    {"%.23s is already playing\nwith someone.", "%.23s ya está jugando\ncon alguien."},
    {"Joined %.23s.\nThe match begins ...", "Unido a %.23s.\nEmpieza la partida ..."},
};

const char *Vita_Tr(const char *english) {
    unsigned k;
    if (english == NULL || !Vita_Spanish()) {
        return english;
    }
    for (k = 0; k < sizeof(kEs) / sizeof(kEs[0]); k++) {
        if (strcmp(kEs[k][0], english) == 0) {
            return kEs[k][1];
        }
    }
    return english;
}
