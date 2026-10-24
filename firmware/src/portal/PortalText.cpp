// Portal page strings; see PortalText.h. English is the member defaults; the
// Spanish table overrides every string by name.
#include "portal/PortalText.h"

#include <cstring>

namespace tgc {

namespace {

PortalText makeSpanish() {
    PortalText t;
    t.htmlLang = "es";

    t.decksNone = "Aún no hay mazos en el dispositivo";
    t.decksOne = "1 mazo en el dispositivo";
    t.decksFmt = "%u mazos en el dispositivo";
    t.freeFmt = "%s libres";
    t.viaFmt = "vía <b>%s</b>";
    t.ownNetFmt = "en la red propia del dispositivo <b>%s</b>";
    t.clockHint = "Abrir esta página pone la fecha y la zona horaria del dispositivo con las de este navegador.";
    t.clockOk = "Fecha del dispositivo tomada de este navegador: ";
    t.clockBad = "El dispositivo no aceptó el reloj de este navegador.";
    t.clockErr = "No se pudo enviar el reloj al dispositivo.";

    t.step1Title = "1. Conecta el dispositivo a tu Wi-Fi";
    t.step1Intro =
        "El dispositivo descarga los mazos por la red de tu casa. Elígela y escribe su "
        "contraseña.";
    t.ssidPlaceholder = "nombre de la red";
    t.passPlaceholder = "contraseña";
    t.save = "Guardar";
    t.passNote =
        "Deja la contraseña vacía si la red es abierta. Se queda en el dispositivo (sin cifrar) "
        "y no se vuelve a mostrar.";
    t.step2Title = "2. Consigue mazos";
    t.step2Text =
        "Con la red guardada, pasa el dispositivo a ella: esta página funcionará desde tu Wi-Fi "
        "de siempre y mostrará los mazos para descargar.";
    t.orUpload = "¿Sin Wi-Fi a mano? Sube un archivo de mazo en Avanzado.";

    t.getDecksTitle = "Consigue mazos";
    t.wifiSavedOk = "Guardada.";
    t.yourWifiFmt = "Tu Wi-Fi: <b>%s</b>";
    t.switchButton = "Pasar a mi Wi-Fi";
    t.switchNoteFmt =
        "El dispositivo deja esta red y se une a <b>%s</b>. Vuelve con el móvil a esa Wi-Fi y "
        "abre la dirección que muestra el dispositivo (o escanea su código QR): ahí está esta "
        "página, con los mazos para descargar.";
    t.deviceGetDecksButton = "Descargar en la pantalla del dispositivo";
    t.deviceGetDecksNote =
        "El dispositivo deja esta red y muestra los mazos en su pantalla: un toque mueve, una "
        "pulsación larga descarga.";

    t.catalogLoading = "Leyendo el catálogo de mazos...";
    t.catalogEmpty = "El catálogo no tiene mazos.";
    t.retry = "Reintentar";
    t.netServer = "No se llega al servidor.";
    t.netTls = "Falló la conexión segura.";
    t.netHttpFmt = "El servidor respondió %d.";
    t.catalogNotPublished = "Aún no hay catálogo publicado en esta URL.";
    t.catalogBad = "Esto no es un catálogo de mazos.";
    t.catalogNewer = "Formato de catálogo más nuevo: actualiza el firmware.";
    t.catalogOlder =
        "Formato de catálogo antiguo: apunta la URL del catálogo (en Avanzado) a un "
        "catalog-v2.txt.";
    t.catalogFailed = "No se pudo leer el catálogo.";
    t.deckInfoFmt = "%s &middot; %u tarjetas &middot; %s";
    t.download = "Descargar";
    t.update = "Actualizar";
    t.onDevice = "En el dispositivo";
    t.studying = "estudiando";
    t.deckNote =
        "Actualizar conserva tu progreso (es de cada kanji, así que los mazos que comparten un "
        "kanji lo comparten). Una actualización necesita sitio para las dos copias un momento.";

    t.dlStartingFmt = "Empezando la descarga de <b>%s</b>...";
    t.dlProgressFmt = "Descargando <b>%s</b>: %u %%";
    t.dlBusyNote =
        "Esta página se actualiza sola. El dispositivo también lo muestra; su botón lateral "
        "cancela.";
    t.dlSavedFmt = "Guardado <b>%s</b>.";
    t.dlNoSpace = "No hay espacio.";
    t.dlNoSpaceUpdate =
        "No hay espacio: una actualización necesita sitio para la copia vieja y la nueva. Borra "
        "antes la vieja en Tus mazos.";
    t.dlCorrupt = "La descarga llegó dañada (tamaño o suma); no se cambió nada.";
    t.dlInvalid = "No es un mazo válido; no se cambió nada.";
    t.dlCancelled = "Cancelado en el dispositivo.";
    t.dlBattery = "Batería baja: carga el dispositivo y vuelve a intentarlo.";
    t.dlFailed = "La descarga falló; no se cambió nada.";
    t.busy = "El dispositivo está ocupado con una descarga; inténtalo cuando acabe.";
    t.unknownDeck = "Ese mazo no está en el catálogo.";

    t.yoursTitle = "Tus mazos";
    t.yoursEmpty = "Aún no hay mazos.";
    t.deleteButton = "Borrar";
    t.deleteConfirm =
        "¿Borrar este mazo y su progreso? Los kanji que también tiene otro mazo del dispositivo "
        "conservan el suyo.";
    t.deletedFmt = "Borrado <b>%s</b>.";
    t.deletedProgressFmt = "Borrado <b>%s</b> y el progreso de %u kanji.";
    t.deleteFailed = "No se pudo borrar.";
    t.badName = "Nombre de mazo no válido.";

    t.advanced = "Avanzado";
    t.uploadTitle = "Subir un archivo de mazo (.tgc)";
    t.uploadButton = "Subir";
    t.uploadOk = "Mazo recibido.";
    t.uploadRejected = "Rechazado:";
    t.upBadName = "nombre de archivo no válido (usa un nombre.tgc corto)";
    t.upTooLarge = "mazo demasiado grande";
    t.upNoSpace = "no hay espacio";
    t.upCannotCreate = "no se puede crear el archivo";
    t.upWriteFailed = "fallo de escritura";
    t.upInvalid = "no es un mazo .tgc válido (descartado)";
    t.upCannotStore = "no se puede guardar el mazo";
    t.upAborted = "subida interrumpida";
    t.progressTitle = "Copia del progreso";
    t.progressDownloadButton = "Descargar el progreso";
    t.progressRestoreButton = "Restaurar progreso";
    t.progressConfirm =
        "¿Reemplazar el progreso de todos los mazos con este archivo? El progreso actual se "
        "guarda en el dispositivo como copia de seguridad.";
    t.progressNote =
        "El archivo tiene el progreso de todos los mazos (el calendario de cada kanji y los "
        "contadores totales). Los ajustes como Wi-Fi, idioma y zona horaria no van incluidos.";
    t.progressSafetyNote =
        "Al restaurar, el archivo anterior se guarda en el dispositivo como "
        "/state/srs.before-restore.bin.";
    t.progressRestored =
        "Progreso restaurado. Al cargar esta página, el dispositivo también ajusta la fecha y la "
        "zona horaria con el navegador.";
    t.progressBad =
        "No restaurado: este no es un archivo de progreso de Tamagotcho válido. No se cambió "
        "nada.";
    t.progressFailed =
        "No restaurado: el dispositivo no pudo guardar el archivo. Tu progreso anterior sigue "
        "cargado.";
    t.progressMissing = "Aún no hay ningún archivo de progreso en el dispositivo.";
    t.urlTitle = "Descargar desde una URL";
    t.urlNoteSetup =
        "Solo http:// simple: en su propia red el dispositivo no tiene internet, así que es "
        "para un servidor en tu móvil u ordenador.";
    t.urlNoteHome =
        "Solo http:// simple, por ejemplo un servidor en tu ordenador. Para los mazos publicados "
        "usa Consigue mazos arriba.";
    t.urlBad = "Solo se admiten URLs http:// simples que acaben en un nombre .tgc.";
    t.urlStartedFmt =
        "Descarga iniciada en <b>%s</b>. Mira la pantalla del dispositivo; esta página muestra "
        "el mazo cuando acabe.";
    t.catalogTitle = "Catálogo de mazos";
    t.catalogPlaceholder = "catálogo por defecto";
    t.catalogDefaultFmt = "Vacío = la publicación de mazos de Tamagotcho: %s";
    t.catalogUrlBad =
        "No guardada: usa una URL http:// o https:// de 256 caracteres como mucho, o déjala "
        "vacía para la de por defecto.";
    t.catalogUrlSaved = "URL del catálogo guardada.";
    t.fwTitle = "Firmware del dispositivo";
    t.fwRunningFmt = "Este dispositivo lleva la <b>%s</b> (compilación %s).";
    t.fwAvailableFmt =
        "Hay firmware nuevo, <b>%s</b>: instálalo en el dispositivo, <b>Ajustes &gt; "
        "Dispositivo &gt; Firmware</b>.";
    t.fwNoOta =
        "La memoria de este dispositivo no tiene segunda partición de app, así que no se "
        "actualiza solo: actualízalo por USB, como se instaló.";
    t.fwUrlPlaceholder = "manifiesto por defecto";
    t.fwUrlDefaultFmt =
        "Vacío = la última versión de Tamagotcho: %s. También vale una URL http://, para probar "
        "un manifiesto e imágenes servidos desde un ordenador.";
    t.fwUrlSaved = "URL del manifiesto de firmware guardada.";
    t.fwUrlBad =
        "No guardada: usa una URL http:// o https:// de 256 caracteres como mucho, o déjala "
        "vacía para el manifiesto por defecto.";
    t.wifiTitle = "Wi-Fi de casa";
    t.savedNetFmt = "Red guardada: <b>%s</b>";
    t.noHomeWifi = "No hay red de casa guardada.";
    t.forget = "Olvidar";
    t.forgetConfirm = "¿Olvidar esta red?";
    t.changeNoteHome =
        "Otra red guardada aquí se usa tras Reconectar, o la próxima vez que se abra el portal.";
    t.reconnect = "Reconectar";
    t.wifiBad =
        "No guardada: el nombre de la red debe tener de 1 a 32 caracteres y la contraseña estar "
        "vacía (red abierta), tener de 8 a 63 caracteres o 64 dígitos hexadecimales.";
    t.forgottenHome =
        "Olvidada. Esta sesión sigue; la próxima vez el portal se abre en modo configuración.";
    t.forgotten = "Olvidada.";
    t.statsTitle = "Estadísticas";
    t.statDate = "fecha";
    t.statEstimated = "(estimada: aún no tomada de un reloj)";
    t.statDay = "día";
    t.statStreak = "racha";
    t.statTime = "tiempo en preguntas";
    t.statSeen = "vistas";
    t.statCards = "tarjetas";
    t.statAnswers = "respuestas";
    t.statAccuracy = "precisión";
    t.statLearned = "aprendidas";
    t.statTrouble = "Kanji más fallados";
    t.statTroubleNote = "Cartas con al menos 3 repasos y un fallo, de peor a mejor.";
    t.statTroubleEmpty = "Aún no hay ninguna carta con 3 repasos y un fallo.";
    t.statRight = "Bien";
    t.statReviews = "Repasos";
    t.freeSpaceFmt = "Espacio libre: %s de %s";
    t.securitySetup =
        "Esta red es <b>abierta</b>: cualquiera a su alcance puede ver, subir o borrar mazos y "
        "leer estas estadísticas mientras la pantalla Wi-Fi está abierta en el dispositivo. "
        "Sal de ella (botón lateral) cuando acabes.";
    t.securityHomeFmt =
        "Esta página no tiene contraseña: cualquiera en <b>%s</b> puede ver, subir o borrar "
        "mazos y leer estas estadísticas mientras la pantalla Wi-Fi está abierta en el "
        "dispositivo. Sal de ella (botón lateral) cuando acabes.";
    t.language = "Idioma:";

    t.back = "Volver";
    t.notFound = "No encontrado.";
    t.noHomeSaved = "Aún no hay Wi-Fi de casa guardada: guarda antes tu red en la página.";
    t.leaveGetDecksTitle = "Descargando mazos por tu Wi-Fi de casa";
    t.leaveGetDecksFmt =
        "El dispositivo deja esta Wi-Fi (<b>%s</b>) para descargar mazos por <b>%s</b>, así que "
        "esta página deja de responder ahora.";
    t.leavePhoneFmt =
        "Puedes desconectar el móvil de <b>%s</b>: vuelve a su propia red (o elígela de nuevo en "
        "sus ajustes de Wi-Fi).";
    t.leaveFollowFmt =
        "<b>Sigue la pantalla del dispositivo.</b> Se une a <b>%s</b> y muestra los mazos: un "
        "toque mueve, una pulsación larga descarga, el botón lateral vuelve.";
    t.leaveAgain =
        "Para usar esta página otra vez, abre <b>Ajustes &gt; Wi-Fi &gt; Portal web</b> en el "
        "dispositivo.";
    t.switchTitle = "Pasando a tu Wi-Fi";
    t.switchLeaveFmt =
        "El dispositivo deja su propia red (<b>%s</b>) para unirse a <b>%s</b>, así que esta "
        "página deja de responder aquí.";
    t.switchPhoneFmt =
        "Vuelve con el móvil a <b>%s</b> (a menudo lo hace solo) y abre la dirección que muestra "
        "el dispositivo, o escanea su código QR.";
    t.switchMdnsFmt = "Suele estar también en <a href=\"%s\">%s</a>.";
    t.switchFail =
        "Si el dispositivo no puede unirse, su pantalla dice por qué; una pulsación larga de su "
        "botón frontal vuelve al modo configuración.";
    t.reconnectTitle = "Reconectando";
    t.reconnectFmt =
        "El dispositivo deja la red y se une a <b>%s</b>, así que esta página deja de responder "
        "un momento. Luego abre la dirección que muestra el dispositivo (puede cambiar).";

    t.footVersionFmt = "Tamagotcho %s &middot; firmware con licencia MIT";
    t.footDecksFmt =
        "Mazos: CC BY-SA. Datos de kanji: %s; orden de trazos: %s; niveles JLPT: %s; glifos: %s; "
        "orden de lecturas: %s; orden de significados: %s.";
    return t;
}

const PortalText kEnglish{};

}  // namespace

const PortalText& portalText(const char* lang) {
    static const PortalText kSpanish = makeSpanish();
    return (lang && std::strcmp(lang, "es") == 0) ? kSpanish : kEnglish;
}

}  // namespace tgc
