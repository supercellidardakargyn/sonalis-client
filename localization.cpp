#include "localization.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ss {
namespace {

using Pack = std::array<const char*, static_cast<std::size_t>(TextId::Count)>;
using DisplayPack = std::array<std::string, static_cast<std::size_t>(TextId::Count)>;

struct ArabicForm final {
    std::uint32_t isolated{};
    std::uint32_t final{};
    std::uint32_t initial{};
    std::uint32_t medial{};
    bool joinsPrevious{};
    bool joinsNext{};
};

constexpr ArabicForm ArabicLetter(const std::uint32_t codepoint) noexcept {
    switch (codepoint) {
        case 0x0621: return {0xFE80, 0, 0, 0, false, false};
        case 0x0622: return {0xFE81, 0xFE82, 0, 0, true, false};
        case 0x0623: return {0xFE83, 0xFE84, 0, 0, true, false};
        case 0x0624: return {0xFE85, 0xFE86, 0, 0, true, false};
        case 0x0625: return {0xFE87, 0xFE88, 0, 0, true, false};
        case 0x0626: return {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, true, true};
        case 0x0627: return {0xFE8D, 0xFE8E, 0, 0, true, false};
        case 0x0628: return {0xFE8F, 0xFE90, 0xFE91, 0xFE92, true, true};
        case 0x0629: return {0xFE93, 0xFE94, 0, 0, true, false};
        case 0x062A: return {0xFE95, 0xFE96, 0xFE97, 0xFE98, true, true};
        case 0x062B: return {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, true, true};
        case 0x062C: return {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, true, true};
        case 0x062D: return {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, true, true};
        case 0x062E: return {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, true, true};
        case 0x062F: return {0xFEA9, 0xFEAA, 0, 0, true, false};
        case 0x0630: return {0xFEAB, 0xFEAC, 0, 0, true, false};
        case 0x0631: return {0xFEAD, 0xFEAE, 0, 0, true, false};
        case 0x0632: return {0xFEAF, 0xFEB0, 0, 0, true, false};
        case 0x0633: return {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, true, true};
        case 0x0634: return {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, true, true};
        case 0x0635: return {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, true, true};
        case 0x0636: return {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, true, true};
        case 0x0637: return {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, true, true};
        case 0x0638: return {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, true, true};
        case 0x0639: return {0xFEC9, 0xFECA, 0xFECB, 0xFECC, true, true};
        case 0x063A: return {0xFECD, 0xFECE, 0xFECF, 0xFED0, true, true};
        case 0x0641: return {0xFED1, 0xFED2, 0xFED3, 0xFED4, true, true};
        case 0x0642: return {0xFED5, 0xFED6, 0xFED7, 0xFED8, true, true};
        case 0x0643: return {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, true, true};
        case 0x0644: return {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, true, true};
        case 0x0645: return {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, true, true};
        case 0x0646: return {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, true, true};
        case 0x0647: return {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, true, true};
        case 0x0648: return {0xFEED, 0xFEEE, 0, 0, true, false};
        case 0x0649: return {0xFEEF, 0xFEF0, 0, 0, true, false};
        case 0x064A: return {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, true, true};
        default: return {};
    }
}

bool ArabicMark(const std::uint32_t value) noexcept {
    return value >= 0x064B && value <= 0x065F;
}

std::vector<std::uint32_t> DecodeUtf8(const std::string_view value) {
    std::vector<std::uint32_t> output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::uint32_t codepoint = 0xFFFD;
        std::size_t length = 1;
        if (first < 0x80) codepoint = first;
        else if ((first & 0xE0) == 0xC0 && index + 1 < value.size()) {
            codepoint = (first & 0x1F) << 6 | (static_cast<unsigned char>(value[index + 1]) & 0x3F); length = 2;
        } else if ((first & 0xF0) == 0xE0 && index + 2 < value.size()) {
            codepoint = (first & 0x0F) << 12 | (static_cast<unsigned char>(value[index + 1]) & 0x3F) << 6
                | (static_cast<unsigned char>(value[index + 2]) & 0x3F); length = 3;
        } else if ((first & 0xF8) == 0xF0 && index + 3 < value.size()) {
            codepoint = (first & 0x07) << 18 | (static_cast<unsigned char>(value[index + 1]) & 0x3F) << 12
                | (static_cast<unsigned char>(value[index + 2]) & 0x3F) << 6
                | (static_cast<unsigned char>(value[index + 3]) & 0x3F); length = 4;
        }
        output.push_back(codepoint);
        index += length;
    }
    return output;
}

void AppendUtf8(std::string& output, const std::uint32_t value) {
    if (value <= 0x7F) output.push_back(static_cast<char>(value));
    else if (value <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | value >> 6));
        output.push_back(static_cast<char>(0x80 | value & 0x3F));
    } else if (value <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | value >> 12));
        output.push_back(static_cast<char>(0x80 | value >> 6 & 0x3F));
        output.push_back(static_cast<char>(0x80 | value & 0x3F));
    } else {
        output.push_back(static_cast<char>(0xF0 | value >> 18));
        output.push_back(static_cast<char>(0x80 | value >> 12 & 0x3F));
        output.push_back(static_cast<char>(0x80 | value >> 6 & 0x3F));
        output.push_back(static_cast<char>(0x80 | value & 0x3F));
    }
}

std::string ShapeArabicForImGui(const std::string_view value) {
    auto codepoints = DecodeUtf8(value);
    const auto logicalCodepoints = codepoints;
    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        const ArabicForm current = ArabicLetter(logicalCodepoints[index]);
        if (current.isolated == 0) continue;
        std::size_t previous = index;
        while (previous > 0 && ArabicMark(logicalCodepoints[previous - 1])) --previous;
        std::size_t next = index + 1;
        while (next < codepoints.size() && ArabicMark(logicalCodepoints[next])) ++next;
        const ArabicForm previousForm = previous > 0 ? ArabicLetter(logicalCodepoints[previous - 1]) : ArabicForm{};
        const ArabicForm nextForm = next < codepoints.size() ? ArabicLetter(logicalCodepoints[next]) : ArabicForm{};
        const bool joinedPrevious = current.joinsPrevious && previousForm.joinsNext;
        const bool joinedNext = current.joinsNext && nextForm.joinsPrevious;
        codepoints[index] = joinedPrevious && joinedNext && current.medial ? current.medial
            : joinedPrevious && current.final ? current.final
            : joinedNext && current.initial ? current.initial : current.isolated;
    }

    std::vector<std::vector<std::uint32_t>> words;
    std::vector<std::uint32_t> word;
    for (const auto codepoint : codepoints) {
        if (codepoint == 0x20 || codepoint == '\n' || codepoint == '\t') {
            if (!word.empty()) { words.push_back(std::move(word)); word.clear(); }
        } else word.push_back(codepoint);
    }
    if (!word.empty()) words.push_back(std::move(word));

    std::string output;
    output.reserve(value.size() * 2);
    for (auto iterator = words.rbegin(); iterator != words.rend(); ++iterator) {
        if (!output.empty()) output.push_back(' ');
        const bool containsArabic = std::ranges::any_of(*iterator, [](const std::uint32_t codepoint) {
            return codepoint >= 0xFB50 && codepoint <= 0xFEFF;
        });
        if (containsArabic) {
            std::vector<std::vector<std::uint32_t>> clusters;
            for (const auto character : *iterator) {
                if (ArabicMark(character) && !clusters.empty()) clusters.back().push_back(character);
                else clusters.push_back({character});
            }
            for (auto cluster = clusters.rbegin(); cluster != clusters.rend(); ++cluster) {
                for (const auto character : *cluster) AppendUtf8(output, character);
            }
        } else {
            for (const auto character : *iterator) AppendUtf8(output, character);
        }
    }
    return output;
}

constexpr Pack kEnglish{
    "Sign in to your Sonalis account", "Your password is never stored by Sonalis. The refresh token is protected by Windows Credential Manager.",
    "CONTROL PLANE", "USERNAME OR EMAIL", "PASSWORD", "Connecting...", "Sign in", "Create an account on the website",
    "Voice", "Rooms", "Messages", "Settings", "Sign out", "Exit Sonalis", "Voice rooms", "Room management",
    "Conversations", "Audio settings", "ROOM", "You are not a member of any room.", "INVITE CODE", "Join with code",
    "Refresh room list", "Create a new room", "Room name", "Create room", "Room members and management",
    "Username", "Search", "Audio devices", "Microphone", "Output", "Refresh devices", "Smart transmission",
    "Automatic voice detection", "Push to talk (hold V)", "Continuous transmission", "Sensitivity", "Connect", "Disconnect",
    "Application settings", "About", "Hide members", "Members", "Unmute microphone", "Mute microphone", "Unmute all",
    "Mute all", "These controls only affect this computer.", "There is no other user in the room.", "Mute",
    "Encrypted room messages", "Encrypted direct message", "Write a message...", "Send", "Ready", "Voice session active",
    "Language", "Language changed", "Up to date", "Checking...", "update", "Downloading...", "Ready to install",
    "Check failed", "Later", "Start installation",
    "Refreshing...", "Refresh members", "Server noise suppression for this room", "Account, room, and node capacity must all be eligible.",
    "DURATION (HOURS)", "MAXIMUM USES", "Create invite code", "Copy", "Member", "Moderator", "Administrator", "Ban", "Cancel",
    "Send request", "Refresh friends", "Accept", "Reject", "Remove", "Block", "Mark read", "Direct P2P in two-person rooms",
    "When enabled, participants can see each other's public IP; relay is used if direct connection fails.", "Test encrypted voice path", "Users",
    "Changes apply without disconnecting.", "Continuous transmission uses Opus and network even in silence, increasing CPU and data use.",
    "Switch to automatic voice detection", "Runtime status", "Export diagnostic report", "MICROPHONE", "Back to room messages",
    "Refresh messages", "Reply", "Edit", "Delete", "Remove as moderator", "Remove message", "A moderation reason is required.",
    "Someone is typing...", "Update ready", "There is no message this device can decrypt.",
};

constexpr Pack kTurkish{
    "Sonalis hesabına gir", "Parolan Sonalis tarafından saklanmaz. Yenileme anahtarı Windows Kimlik Bilgisi Yöneticisi'nde korunur.",
    "MERKEZİ SİSTEM", "KULLANICI ADI VEYA E-POSTA", "PAROLA", "Bağlanıyor...", "Giriş yap", "Web sitesinde hesap oluştur",
    "Ses", "Odalar", "Mesajlar", "Ayarlar", "Oturumu kapat", "Sonalis'ten çık", "Sesli odalar", "Oda yönetimi",
    "Konuşmalar", "Ses ayarları", "ODA", "Üyesi olduğunuz oda yok.", "DAVET KODU", "Kodla katıl",
    "Oda listesini yenile", "Yeni oda oluştur", "Oda adı", "Odayı oluştur", "Oda üyeleri ve yönetim",
    "Kullanıcı adı", "Ara", "Ses cihazları", "Mikrofon", "Çıkış", "Cihazları yenile", "Akıllı gönderim",
    "Otomatik konuşma algılama", "Bas-konuş (V basılı tut)", "Sürekli aktarım", "Hassasiyet", "Bağlan", "Bağlantıyı kes",
    "Uygulama ayarları", "Hakkında", "Üyeleri gizle", "Üyeler", "Mikrofonu aç", "Mikrofonu kapat", "Tüm sesi aç",
    "Tüm sesi kapat", "Bu kontroller yalnızca bu bilgisayarı etkiler.", "Odada başka kullanıcı yok.", "Sustur",
    "Şifreli oda mesajları", "Şifreli özel mesaj", "Mesaj yaz...", "Gönder", "Hazır", "Ses oturumu etkin",
    "Dil", "Dil değiştirildi", "Güncel", "Denetleniyor...", "güncellemesi", "İndiriliyor...", "Kuruluma hazır",
    "Kontrol edilemedi", "Daha sonra", "Kurulumu başlat",
    "Yenileniyor...", "Üyeleri yenile", "Bu oda için sunucu gürültü engelleme", "Hesap, oda ve düğüm kapasitesinin tümü uygun olmalıdır.",
    "SÜRE (SAAT)", "AZAMİ KULLANIM", "Davet kodu oluştur", "Kopyala", "Üye", "Moderatör", "Yönetici", "Banla", "Vazgeç",
    "İstek gönder", "Arkadaşları yenile", "Kabul et", "Reddet", "Çıkar", "Engelle", "Okundu işaretle", "İki kişilik odalarda doğrudan P2P",
    "Açıkken katılımcılar birbirlerinin genel IP adresini görebilir; doğrudan bağlantı kurulamazsa relay kullanılır.", "Şifreli ses yolunu test et", "Kullanıcılar",
    "Değişiklik bağlantı kesilmeden uygulanır.", "Sürekli aktarım sessizlikte de Opus ve ağı kullanır; CPU ve veri tüketimini artırır.",
    "Otomatik konuşma algılamaya geç", "Çalışma durumu", "Tanılama raporunu dışa aktar", "MİKROFON", "Oda mesajlarına dön",
    "Mesajları yenile", "Yanıtla", "Düzenle", "Sil", "Moderatör olarak kaldır", "Mesajı kaldır", "Moderasyon gerekçesi zorunludur.",
    "Bir kullanıcı yazıyor...", "Güncelleme hazır", "Bu cihazın çözebildiği mesaj yok.",
};

constexpr Pack kGerman{
    "Bei Sonalis anmelden", "Dein Passwort wird von Sonalis nicht gespeichert. Das Aktualisierungstoken wird von Windows geschützt.",
    "ZENTRALES SYSTEM", "BENUTZERNAME ODER E-MAIL", "PASSWORT", "Verbindung...", "Anmelden", "Konto auf der Website erstellen",
    "Sprache", "Räume", "Nachrichten", "Einstellungen", "Abmelden", "Sonalis beenden", "Sprachräume", "Raumverwaltung",
    "Unterhaltungen", "Audioeinstellungen", "RAUM", "Du bist noch keinem Raum beigetreten.", "EINLADUNGSCODE", "Mit Code beitreten",
    "Raumliste aktualisieren", "Neuen Raum erstellen", "Raumname", "Raum erstellen", "Mitglieder und Verwaltung",
    "Benutzername", "Suchen", "Audiogeräte", "Mikrofon", "Ausgabe", "Geräte aktualisieren", "Intelligente Übertragung",
    "Automatische Spracherkennung", "Push-to-Talk (V halten)", "Dauerübertragung", "Empfindlichkeit", "Verbinden", "Trennen",
    "Anwendungseinstellungen", "Über", "Mitglieder ausblenden", "Mitglieder", "Mikrofon einschalten", "Mikrofon stummschalten", "Ton einschalten",
    "Alles stummschalten", "Diese Steuerung betrifft nur diesen Computer.", "Keine weitere Person im Raum.", "Stummschalten",
    "Verschlüsselte Raumnachrichten", "Verschlüsselte Direktnachricht", "Nachricht schreiben...", "Senden", "Bereit", "Sprachsitzung aktiv",
    "Sprache", "Sprache geändert", "Aktuell", "Wird geprüft...", "Aktualisierung", "Wird heruntergeladen...", "Installationsbereit",
    "Prüfung fehlgeschlagen", "Später", "Installation starten",
    "Wird aktualisiert...", "Mitglieder aktualisieren", "Server-Rauschunterdrückung für diesen Raum", "Konto, Raum und Knotenkapazität müssen geeignet sein.",
    "DAUER (STUNDEN)", "MAXIMALE NUTZUNGEN", "Einladungscode erstellen", "Kopieren", "Mitglied", "Moderator", "Administrator", "Sperren", "Abbrechen",
    "Anfrage senden", "Freunde aktualisieren", "Annehmen", "Ablehnen", "Entfernen", "Blockieren", "Als gelesen markieren", "Direktes P2P in Räumen mit zwei Personen",
    "Wenn aktiviert, können Teilnehmer die öffentliche IP des anderen sehen; bei Fehler wird Relay verwendet.", "Verschlüsselten Sprachpfad testen", "Benutzer",
    "Änderungen werden ohne Trennung angewendet.", "Dauerübertragung nutzt auch bei Stille Opus und Netzwerk und erhöht CPU- und Datenverbrauch.",
    "Zur automatischen Spracherkennung wechseln", "Betriebsstatus", "Diagnosebericht exportieren", "MIKROFON", "Zurück zu Raumnachrichten",
    "Nachrichten aktualisieren", "Antworten", "Bearbeiten", "Löschen", "Als Moderator entfernen", "Nachricht entfernen", "Ein Moderationsgrund ist erforderlich.",
    "Jemand schreibt...", "Update bereit", "Auf diesem Gerät ist keine entschlüsselbare Nachricht vorhanden.",
};

constexpr Pack kSpanish{
    "Inicia sesión en Sonalis", "Sonalis no guarda tu contraseña. Windows protege el token de actualización.",
    "SISTEMA CENTRAL", "USUARIO O CORREO", "CONTRASEÑA", "Conectando...", "Iniciar sesión", "Crear una cuenta en la web",
    "Voz", "Salas", "Mensajes", "Ajustes", "Cerrar sesión", "Salir de Sonalis", "Salas de voz", "Gestión de sala",
    "Conversaciones", "Ajustes de audio", "SALA", "No perteneces a ninguna sala.", "CÓDIGO DE INVITACIÓN", "Unirse con código",
    "Actualizar salas", "Crear una sala", "Nombre de la sala", "Crear sala", "Miembros y gestión",
    "Usuario", "Buscar", "Dispositivos de audio", "Micrófono", "Salida", "Actualizar dispositivos", "Transmisión inteligente",
    "Detección automática de voz", "Pulsar para hablar (mantén V)", "Transmisión continua", "Sensibilidad", "Conectar", "Desconectar",
    "Ajustes de la aplicación", "Acerca de", "Ocultar miembros", "Miembros", "Activar micrófono", "Silenciar micrófono", "Activar todo",
    "Silenciar todo", "Estos controles solo afectan a este equipo.", "No hay otra persona en la sala.", "Silenciar",
    "Mensajes cifrados de sala", "Mensaje directo cifrado", "Escribe un mensaje...", "Enviar", "Listo", "Sesión de voz activa",
    "Idioma", "Idioma cambiado", "Actualizado", "Comprobando...", "actualización", "Descargando...", "Listo para instalar",
    "No se pudo comprobar", "Más tarde", "Iniciar instalación",
    "Actualizando...", "Actualizar miembros", "Supresión de ruido del servidor para esta sala", "La cuenta, la sala y la capacidad del nodo deben ser compatibles.",
    "DURACIÓN (HORAS)", "USOS MÁXIMOS", "Crear código de invitación", "Copiar", "Miembro", "Moderador", "Administrador", "Bloquear acceso", "Cancelar",
    "Enviar solicitud", "Actualizar amigos", "Aceptar", "Rechazar", "Eliminar", "Bloquear", "Marcar como leído", "P2P directo en salas de dos personas",
    "Al activarlo, los participantes pueden ver la IP pública del otro; si falla, se usa el relay.", "Probar ruta de voz cifrada", "Usuarios",
    "Los cambios se aplican sin desconectar.", "La transmisión continua usa Opus y la red incluso en silencio, aumentando el uso de CPU y datos.",
    "Cambiar a detección automática de voz", "Estado de ejecución", "Exportar informe de diagnóstico", "MICRÓFONO", "Volver a mensajes de la sala",
    "Actualizar mensajes", "Responder", "Editar", "Eliminar", "Eliminar como moderador", "Eliminar mensaje", "Se requiere un motivo de moderación.",
    "Alguien está escribiendo...", "Actualización lista", "Este dispositivo no puede descifrar ningún mensaje.",
};

constexpr Pack kFrench{
    "Se connecter à Sonalis", "Sonalis ne stocke jamais votre mot de passe. Windows protège le jeton de renouvellement.",
    "SYSTÈME CENTRAL", "NOM D'UTILISATEUR OU E-MAIL", "MOT DE PASSE", "Connexion...", "Se connecter", "Créer un compte sur le site",
    "Voix", "Salons", "Messages", "Paramètres", "Se déconnecter", "Quitter Sonalis", "Salons vocaux", "Gestion du salon",
    "Conversations", "Paramètres audio", "SALON", "Vous n'êtes membre d'aucun salon.", "CODE D'INVITATION", "Rejoindre avec le code",
    "Actualiser les salons", "Créer un salon", "Nom du salon", "Créer", "Membres et gestion",
    "Nom d'utilisateur", "Rechercher", "Périphériques audio", "Microphone", "Sortie", "Actualiser les périphériques", "Transmission intelligente",
    "Détection vocale automatique", "Appuyer pour parler (maintenir V)", "Transmission continue", "Sensibilité", "Se connecter", "Déconnecter",
    "Paramètres de l'application", "À propos", "Masquer les membres", "Membres", "Activer le microphone", "Couper le microphone", "Activer le son",
    "Tout couper", "Ces contrôles n'affectent que cet ordinateur.", "Aucun autre utilisateur dans le salon.", "Muet",
    "Messages chiffrés du salon", "Message direct chiffré", "Écrire un message...", "Envoyer", "Prêt", "Session vocale active",
    "Langue", "Langue modifiée", "À jour", "Vérification...", "mise à jour", "Téléchargement...", "Prêt à installer",
    "Échec de la vérification", "Plus tard", "Lancer l'installation",
    "Actualisation...", "Actualiser les membres", "Réduction du bruit serveur pour ce salon", "Le compte, le salon et la capacité du nœud doivent être éligibles.",
    "DURÉE (HEURES)", "UTILISATIONS MAX.", "Créer un code d'invitation", "Copier", "Membre", "Modérateur", "Administrateur", "Bannir", "Annuler",
    "Envoyer la demande", "Actualiser les amis", "Accepter", "Refuser", "Retirer", "Bloquer", "Marquer comme lu", "P2P direct dans les salons à deux personnes",
    "Si activé, chaque participant peut voir l'IP publique de l'autre ; le relais est utilisé en cas d'échec.", "Tester le chemin vocal chiffré", "Utilisateurs",
    "Les modifications s'appliquent sans déconnexion.", "La transmission continue utilise Opus et le réseau même en silence, augmentant l'usage CPU et des données.",
    "Passer à la détection vocale automatique", "État d'exécution", "Exporter le rapport de diagnostic", "MICROPHONE", "Retour aux messages du salon",
    "Actualiser les messages", "Répondre", "Modifier", "Supprimer", "Retirer comme modérateur", "Retirer le message", "Un motif de modération est requis.",
    "Quelqu'un écrit...", "Mise à jour prête", "Cet appareil ne peut déchiffrer aucun message.",
};

constexpr Pack kPortuguese{
    "Entrar no Sonalis", "A Sonalis não armazena a sua senha. O Windows protege o token de renovação.",
    "SISTEMA CENTRAL", "UTILIZADOR OU E-MAIL", "SENHA", "A ligar...", "Entrar", "Criar conta no site",
    "Voz", "Salas", "Mensagens", "Definições", "Terminar sessão", "Sair do Sonalis", "Salas de voz", "Gestão da sala",
    "Conversas", "Definições de áudio", "SALA", "Ainda não pertence a nenhuma sala.", "CÓDIGO DE CONVITE", "Entrar com código",
    "Atualizar salas", "Criar nova sala", "Nome da sala", "Criar sala", "Membros e gestão",
    "Utilizador", "Procurar", "Dispositivos de áudio", "Microfone", "Saída", "Atualizar dispositivos", "Transmissão inteligente",
    "Deteção automática de voz", "Premir para falar (manter V)", "Transmissão contínua", "Sensibilidade", "Ligar", "Desligar",
    "Definições da aplicação", "Sobre", "Ocultar membros", "Membros", "Ativar microfone", "Silenciar microfone", "Ativar som",
    "Silenciar tudo", "Estes controlos afetam apenas este computador.", "Não há outro utilizador na sala.", "Silenciar",
    "Mensagens de sala cifradas", "Mensagem direta cifrada", "Escrever mensagem...", "Enviar", "Pronto", "Sessão de voz ativa",
    "Idioma", "Idioma alterado", "Atualizado", "A verificar...", "atualização", "A transferir...", "Pronto para instalar",
    "Falha na verificação", "Mais tarde", "Iniciar instalação",
    "A atualizar...", "Atualizar membros", "Supressão de ruído no servidor para esta sala", "A conta, a sala e a capacidade do nó têm de ser elegíveis.",
    "DURAÇÃO (HORAS)", "UTILIZAÇÕES MÁX.", "Criar código de convite", "Copiar", "Membro", "Moderador", "Administrador", "Banir", "Cancelar",
    "Enviar pedido", "Atualizar amigos", "Aceitar", "Recusar", "Remover", "Bloquear", "Marcar como lida", "P2P direto em salas com duas pessoas",
    "Quando ativo, os participantes podem ver o IP público um do outro; se falhar, é usado o relay.", "Testar caminho de voz cifrado", "Utilizadores",
    "As alterações são aplicadas sem desligar.", "A transmissão contínua usa Opus e a rede mesmo em silêncio, aumentando o uso de CPU e dados.",
    "Mudar para deteção automática de voz", "Estado de execução", "Exportar relatório de diagnóstico", "MICROFONE", "Voltar às mensagens da sala",
    "Atualizar mensagens", "Responder", "Editar", "Eliminar", "Remover como moderador", "Remover mensagem", "É necessário um motivo de moderação.",
    "Alguém está a escrever...", "Atualização pronta", "Este dispositivo não consegue decifrar nenhuma mensagem.",
};

constexpr Pack kRussian{
    "Войти в Sonalis", "Sonalis не хранит ваш пароль. Токен обновления защищён диспетчером учётных данных Windows.",
    "ЦЕНТРАЛЬНАЯ СИСТЕМА", "ИМЯ ПОЛЬЗОВАТЕЛЯ ИЛИ E-MAIL", "ПАРОЛЬ", "Подключение...", "Войти", "Создать аккаунт на сайте",
    "Голос", "Комнаты", "Сообщения", "Настройки", "Выйти из аккаунта", "Закрыть Sonalis", "Голосовые комнаты", "Управление комнатой",
    "Диалоги", "Настройки звука", "КОМНАТА", "Вы не состоите ни в одной комнате.", "КОД ПРИГЛАШЕНИЯ", "Войти по коду",
    "Обновить список", "Создать комнату", "Название комнаты", "Создать", "Участники и управление",
    "Имя пользователя", "Поиск", "Аудиоустройства", "Микрофон", "Вывод", "Обновить устройства", "Умная передача",
    "Автоматическое определение речи", "Нажми и говори (удерживать V)", "Непрерывная передача", "Чувствительность", "Подключиться", "Отключиться",
    "Настройки приложения", "О программе", "Скрыть участников", "Участники", "Включить микрофон", "Выключить микрофон", "Включить звук",
    "Выключить весь звук", "Эти настройки действуют только на этом компьютере.", "В комнате больше никого нет.", "Без звука",
    "Зашифрованные сообщения комнаты", "Зашифрованное личное сообщение", "Написать сообщение...", "Отправить", "Готово", "Голосовая сессия активна",
    "Язык", "Язык изменён", "Актуально", "Проверка...", "обновление", "Загрузка...", "Готово к установке",
    "Ошибка проверки", "Позже", "Начать установку",
    "Обновление...", "Обновить участников", "Серверное шумоподавление для этой комнаты", "Учётная запись, комната и узел должны поддерживать эту функцию.",
    "СРОК (ЧАСЫ)", "МАКС. ИСПОЛЬЗОВАНИЙ", "Создать код приглашения", "Копировать", "Участник", "Модератор", "Администратор", "Заблокировать", "Отмена",
    "Отправить запрос", "Обновить друзей", "Принять", "Отклонить", "Удалить", "Заблокировать", "Отметить прочитанным", "Прямое P2P для двух участников",
    "При включении участники видят публичные IP друг друга; при ошибке используется ретранслятор.", "Проверить зашифрованный голосовой канал", "Пользователи",
    "Изменения применяются без отключения.", "Непрерывная передача использует Opus и сеть даже в тишине, повышая нагрузку на ЦП и расход данных.",
    "Перейти к автоматическому распознаванию речи", "Состояние работы", "Экспортировать отчёт диагностики", "МИКРОФОН", "Назад к сообщениям комнаты",
    "Обновить сообщения", "Ответить", "Изменить", "Удалить", "Удалить как модератор", "Удалить сообщение", "Укажите причину модерации.",
    "Кто-то печатает...", "Обновление готово", "На этом устройстве нет доступных для расшифровки сообщений.",
};

constexpr Pack kItalian{
    "Accedi al tuo account Sonalis", "Sonalis non memorizza mai la password. Il token di rinnovo è protetto da Gestione credenziali di Windows.",
    "SISTEMA CENTRALE", "NOME UTENTE O E-MAIL", "PASSWORD", "Connessione...", "Accedi", "Crea un account sul sito web",
    "Voce", "Stanze", "Messaggi", "Impostazioni", "Disconnetti", "Esci da Sonalis", "Stanze vocali", "Gestione stanza",
    "Conversazioni", "Impostazioni audio", "STANZA", "Non sei membro di alcuna stanza.", "CODICE INVITO", "Entra con il codice",
    "Aggiorna elenco stanze", "Crea una nuova stanza", "Nome stanza", "Crea stanza", "Membri e gestione della stanza",
    "Nome utente", "Cerca", "Dispositivi audio", "Microfono", "Uscita", "Aggiorna dispositivi", "Trasmissione intelligente",
    "Rilevamento automatico della voce", "Premi per parlare (tieni premuto V)", "Trasmissione continua", "Sensibilità", "Connetti", "Disconnetti",
    "Impostazioni applicazione", "Informazioni", "Nascondi membri", "Membri", "Attiva microfono", "Disattiva microfono", "Attiva tutto l'audio",
    "Disattiva tutto l'audio", "Questi controlli hanno effetto solo su questo computer.", "Non ci sono altri utenti nella stanza.", "Disattiva audio",
    "Messaggi della stanza crittografati", "Messaggio diretto crittografato", "Scrivi un messaggio...", "Invia", "Pronto", "Sessione vocale attiva",
    "Lingua", "Lingua modificata", "Aggiornato", "Controllo...", "aggiornamento", "Download...", "Pronto per l'installazione",
    "Controllo non riuscito", "Più tardi", "Avvia installazione",
    "Aggiornamento...", "Aggiorna membri", "Riduzione rumore sul server per questa stanza", "Account, stanza e capacità del nodo devono essere idonei.",
    "DURATA (ORE)", "UTILIZZI MASSIMI", "Crea codice invito", "Copia", "Membro", "Moderatore", "Amministratore", "Banna", "Annulla",
    "Invia richiesta", "Aggiorna amici", "Accetta", "Rifiuta", "Rimuovi", "Blocca", "Segna come letto", "P2P diretto nelle stanze con due persone",
    "Se attivo, i partecipanti possono vedere l'IP pubblico dell'altro; in caso di errore viene usato il relay.", "Testa percorso vocale cifrato", "Utenti",
    "Le modifiche si applicano senza disconnettersi.", "La trasmissione continua usa Opus e la rete anche nel silenzio, aumentando l'uso di CPU e dati.",
    "Passa al rilevamento vocale automatico", "Stato di esecuzione", "Esporta rapporto diagnostico", "MICROFONO", "Torna ai messaggi della stanza",
    "Aggiorna messaggi", "Rispondi", "Modifica", "Elimina", "Rimuovi come moderatore", "Rimuovi messaggio", "È necessario un motivo di moderazione.",
    "Qualcuno sta scrivendo...", "Aggiornamento pronto", "Questo dispositivo non può decifrare alcun messaggio.",
};

constexpr Pack kArabic{
    "تسجيل الدخول إلى حساب Sonalis", "لا يخزّن Sonalis كلمة مرورك. يحمي مدير بيانات اعتماد Windows رمز التجديد.",
    "النظام المركزي", "اسم المستخدم أو البريد الإلكتروني", "كلمة المرور", "جارٍ الاتصال...", "تسجيل الدخول", "إنشاء حساب على الموقع",
    "الصوت", "الغرف", "الرسائل", "الإعدادات", "تسجيل الخروج", "الخروج من Sonalis", "الغرف الصوتية", "إدارة الغرفة",
    "المحادثات", "إعدادات الصوت", "الغرفة", "لست عضوًا في أي غرفة.", "رمز الدعوة", "الانضمام بالرمز",
    "تحديث قائمة الغرف", "إنشاء غرفة جديدة", "اسم الغرفة", "إنشاء الغرفة", "أعضاء الغرفة وإدارتها",
    "اسم المستخدم", "بحث", "أجهزة الصوت", "الميكروفون", "الإخراج", "تحديث الأجهزة", "الإرسال الذكي",
    "اكتشاف الصوت تلقائيًا", "اضغط للتحدث (استمر بالضغط على V)", "إرسال مستمر", "الحساسية", "اتصال", "قطع الاتصال",
    "إعدادات التطبيق", "حول", "إخفاء الأعضاء", "الأعضاء", "تشغيل الميكروفون", "كتم الميكروفون", "تشغيل كل الأصوات",
    "كتم كل الأصوات", "تؤثر عناصر التحكم هذه في هذا الكمبيوتر فقط.", "لا يوجد مستخدم آخر في الغرفة.", "كتم",
    "رسائل الغرفة المشفّرة", "رسالة خاصة مشفّرة", "اكتب رسالة...", "إرسال", "جاهز", "الجلسة الصوتية نشطة",
    "اللغة", "تم تغيير اللغة", "محدّث", "جارٍ التحقق...", "تحديث", "جارٍ التنزيل...", "جاهز للتثبيت",
    "تعذر التحقق", "لاحقًا", "بدء التثبيت",
    "جارٍ التحديث...", "تحديث الأعضاء", "إزالة ضوضاء الخادم لهذه الغرفة", "يجب أن يكون الحساب والغرفة وسعة العقدة مؤهلة.",
    "المدة (ساعات)", "الحد الأقصى للاستخدام", "إنشاء رمز دعوة", "نسخ", "عضو", "مشرف", "مسؤول", "حظر", "إلغاء",
    "إرسال الطلب", "تحديث الأصدقاء", "قبول", "رفض", "إزالة", "حظر", "تعليم كمقروء", "اتصال P2P مباشر في غرف شخصين",
    "عند التفعيل يمكن للمشاركين رؤية عنوان IP العام لبعضهم؛ ويُستخدم المرحّل عند فشل الاتصال المباشر.", "اختبار مسار الصوت المشفر", "المستخدمون",
    "تُطبّق التغييرات دون قطع الاتصال.", "يستخدم الإرسال المستمر Opus والشبكة حتى أثناء الصمت، مما يزيد استخدام المعالج والبيانات.",
    "التبديل إلى اكتشاف الصوت التلقائي", "حالة التشغيل", "تصدير تقرير التشخيص", "الميكروفون", "العودة إلى رسائل الغرفة",
    "تحديث الرسائل", "رد", "تعديل", "حذف", "إزالة كمشرف", "إزالة الرسالة", "سبب الإشراف مطلوب.",
    "شخص ما يكتب...", "التحديث جاهز", "لا توجد رسالة يمكن لهذا الجهاز فك تشفيرها.",
};

constexpr Pack kJapanese{
    "Sonalis アカウントにサインイン", "Sonalis はパスワードを保存しません。更新トークンは Windows 資格情報マネージャーで保護されます。",
    "中央システム", "ユーザー名またはメール", "パスワード", "接続中...", "サインイン", "Web サイトでアカウントを作成",
    "音声", "ルーム", "メッセージ", "設定", "サインアウト", "Sonalis を終了", "音声ルーム", "ルーム管理",
    "会話", "オーディオ設定", "ルーム", "参加しているルームはありません。", "招待コード", "コードで参加",
    "ルーム一覧を更新", "新しいルームを作成", "ルーム名", "ルームを作成", "メンバーとルーム管理",
    "ユーザー名", "検索", "オーディオデバイス", "マイク", "出力", "デバイスを更新", "スマート送信",
    "自動音声検出", "プッシュトゥトーク（V を長押し）", "常時送信", "感度", "接続", "切断",
    "アプリ設定", "情報", "メンバーを隠す", "メンバー", "マイクをオン", "マイクをミュート", "すべての音声をオン",
    "すべての音声をミュート", "これらの操作はこのコンピューターにのみ影響します。", "ルームにほかのユーザーはいません。", "ミュート",
    "暗号化されたルームメッセージ", "暗号化されたダイレクトメッセージ", "メッセージを入力...", "送信", "準備完了", "音声セッション有効",
    "言語", "言語を変更しました", "最新", "確認中...", "アップデート", "ダウンロード中...", "インストール準備完了",
    "確認できませんでした", "後で", "インストールを開始",
    "更新中...", "メンバーを更新", "このルームでサーバーノイズ抑制を使用", "アカウント、ルーム、ノード容量のすべてが条件を満たす必要があります。",
    "期間（時間）", "最大使用回数", "招待コードを作成", "コピー", "メンバー", "モデレーター", "管理者", "禁止", "キャンセル",
    "リクエストを送信", "友達を更新", "承認", "拒否", "削除", "ブロック", "既読にする", "2人ルームで直接P2P",
    "有効にすると参加者は互いの公開IPを確認できます。直接接続に失敗した場合はリレーを使用します。", "暗号化音声経路をテスト", "ユーザー",
    "変更は切断せずに適用されます。", "常時送信は無音時も Opus とネットワークを使用し、CPU とデータ使用量が増えます。",
    "自動音声検出に切り替える", "動作状態", "診断レポートを書き出す", "マイク", "ルームメッセージに戻る",
    "メッセージを更新", "返信", "編集", "削除", "モデレーターとして削除", "メッセージを削除", "モデレーション理由が必要です。",
    "入力中です...", "更新の準備完了", "この端末で復号できるメッセージはありません。",
};

constexpr Pack kKorean{
    "Sonalis 계정에 로그인", "Sonalis는 비밀번호를 저장하지 않습니다. 갱신 토큰은 Windows 자격 증명 관리자에서 보호됩니다.",
    "중앙 시스템", "사용자 이름 또는 이메일", "비밀번호", "연결 중...", "로그인", "웹사이트에서 계정 만들기",
    "음성", "방", "메시지", "설정", "로그아웃", "Sonalis 종료", "음성 채널", "방 관리",
    "대화", "오디오 설정", "방", "가입한 방이 없습니다.", "초대 코드", "코드로 참여",
    "방 목록 새로고침", "새 방 만들기", "방 이름", "방 만들기", "멤버 및 방 관리",
    "사용자 이름", "검색", "오디오 장치", "마이크", "출력", "장치 새로고침", "스마트 전송",
    "자동 음성 감지", "눌러서 말하기(V 길게 누르기)", "항상 전송", "감도", "연결", "연결 끊기",
    "앱 설정", "정보", "멤버 숨기기", "멤버", "마이크 켜기", "마이크 음소거", "모든 소리 켜기",
    "모든 소리 음소거", "이 설정은 이 컴퓨터에만 적용됩니다.", "방에 다른 사용자가 없습니다.", "음소거",
    "암호화된 방 메시지", "암호화된 개인 메시지", "메시지 입력...", "보내기", "준비됨", "음성 세션 활성",
    "언어", "언어가 변경됨", "최신 버전", "확인 중...", "업데이트", "다운로드 중...", "설치 준비됨",
    "확인 실패", "나중에", "설치 시작",
    "새로 고치는 중...", "멤버 새로 고침", "이 방에 서버 소음 제거 사용", "계정, 방 및 노드 용량이 모두 조건을 충족해야 합니다.",
    "기간(시간)", "최대 사용 횟수", "초대 코드 만들기", "복사", "멤버", "중재자", "관리자", "차단", "취소",
    "요청 보내기", "친구 새로 고침", "수락", "거절", "삭제", "차단", "읽음으로 표시", "2인 방에서 직접 P2P",
    "활성화하면 참가자가 서로의 공인 IP를 볼 수 있으며 직접 연결 실패 시 릴레이를 사용합니다.", "암호화된 음성 경로 테스트", "사용자",
    "연결을 끊지 않고 변경 사항이 적용됩니다.", "계속 전송은 무음에서도 Opus와 네트워크를 사용하므로 CPU 및 데이터 사용량이 증가합니다.",
    "자동 음성 감지로 전환", "실행 상태", "진단 보고서 내보내기", "마이크", "방 메시지로 돌아가기",
    "메시지 새로고침", "답장", "편집", "삭제", "관리자로 삭제", "메시지 삭제", "관리 사유가 필요합니다.",
    "입력 중인 사용자가 있습니다...", "업데이트 준비됨", "이 기기에서 복호화할 수 있는 메시지가 없습니다.",
};

constexpr Pack kSimplifiedChinese{
    "登录 Sonalis 帐户", "Sonalis 不会存储你的密码。刷新令牌由 Windows 凭据管理器保护。",
    "中央系统", "用户名或电子邮箱", "密码", "正在连接...", "登录", "在网站上创建帐户",
    "语音", "房间", "消息", "设置", "退出登录", "退出 Sonalis", "语音房间", "房间管理",
    "会话", "音频设置", "房间", "你尚未加入任何房间。", "邀请码", "使用代码加入",
    "刷新房间列表", "创建新房间", "房间名称", "创建房间", "成员和房间管理",
    "用户名", "搜索", "音频设备", "麦克风", "输出", "刷新设备", "智能传输",
    "自动语音检测", "按住说话（按住 V）", "持续传输", "灵敏度", "连接", "断开连接",
    "应用设置", "关于", "隐藏成员", "成员", "打开麦克风", "麦克风静音", "打开全部声音",
    "全部静音", "这些控制只影响此计算机。", "房间中没有其他用户。", "静音",
    "加密房间消息", "加密私信", "输入消息...", "发送", "就绪", "语音会话已启用",
    "语言", "语言已更改", "已是最新", "正在检查...", "更新", "正在下载...", "可以安装",
    "检查失败", "稍后", "开始安装",
    "正在刷新...", "刷新成员", "为此房间启用服务器降噪", "帐户、房间和节点容量都必须符合条件。",
    "时长（小时）", "最大使用次数", "创建邀请码", "复制", "成员", "版主", "管理员", "封禁", "取消",
    "发送请求", "刷新好友", "接受", "拒绝", "移除", "屏蔽", "标记为已读", "双人房间直接 P2P",
    "启用后参与者可看到对方的公网 IP；直接连接失败时使用中继。", "测试加密语音路径", "用户",
    "更改无需断开连接即可生效。", "持续传输在静音时也使用 Opus 和网络，会增加 CPU 与流量消耗。",
    "切换到自动语音检测", "运行状态", "导出诊断报告", "麦克风", "返回房间消息",
    "刷新消息", "回复", "编辑", "删除", "以管理员身份删除", "删除消息", "必须填写管理原因。",
    "有人正在输入...", "更新已就绪", "此设备上没有可解密的消息。",
};

constexpr std::array<LanguageOption, 12> kLanguages{{
    {Language::English, "en", "English"}, {Language::Turkish, "tr", "Türkçe"},
    {Language::German, "de", "Deutsch"}, {Language::Spanish, "es", "Español"},
    {Language::French, "fr", "Français"}, {Language::Portuguese, "pt-BR", "Português (Brasil)"},
    {Language::Italian, "it", "Italiano"}, {Language::Russian, "ru", "Русский"},
    {Language::Arabic, "ar", "العربية"}, {Language::Japanese, "ja", "日本語"},
    {Language::Korean, "ko", "한국어"}, {Language::SimplifiedChinese, "zh-Hans", "简体中文"},
}};

const DisplayPack& ArabicDisplayPack() {
    static const DisplayPack display = [] {
        DisplayPack result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = ShapeArabicForImGui(kArabic[index]);
        }
        return result;
    }();
    return display;
}

const Pack& SelectPack(const Language language) noexcept {
    switch (language) {
        case Language::Turkish: return kTurkish;
        case Language::German: return kGerman;
        case Language::Spanish: return kSpanish;
        case Language::French: return kFrench;
        case Language::Portuguese: return kPortuguese;
        case Language::Italian: return kItalian;
        case Language::Russian: return kRussian;
        case Language::Arabic: return kArabic;
        case Language::Japanese: return kJapanese;
        case Language::Korean: return kKorean;
        case Language::SimplifiedChinese: return kSimplifiedChinese;
        case Language::English: return kEnglish;
    }
    return kEnglish;
}

}  // namespace

const std::array<LanguageOption, 12>& SupportedLanguages() noexcept { return kLanguages; }
Language ParseLanguage(const std::string_view code) noexcept {
    for (const auto& option : kLanguages) if (option.code == code) return option.language;
    return Language::English;
}
std::string_view LanguageCode(const Language language) noexcept {
    for (const auto& option : kLanguages) if (option.language == language) return option.code;
    return "en";
}
const char* LanguageDisplayName(const Language language) noexcept {
    for (const auto& option : kLanguages) {
        if (option.language != language) continue;
        if (language != Language::Arabic) return option.nativeName.data();
        static const std::string arabic = ShapeArabicForImGui(option.nativeName);
        return arabic.c_str();
    }
    return "English";
}
const char* Translate(const Language language, const TextId id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(TextId::Count)) return "";
    if (language == Language::Arabic) return ArabicDisplayPack()[index].c_str();
    return SelectPack(language)[index];
}
bool IsRightToLeft(const Language language) noexcept { return language == Language::Arabic; }

}  // namespace ss
