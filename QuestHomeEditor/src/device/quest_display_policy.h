#pragma once

#include <cctype>
#include <string>

namespace questdisplay {

enum class Model {
    Unknown,
    Quest1,
    Quest2,
    QuestPro,
    Quest3,
    Quest3S,
};

struct DeviceIdentity {
    std::string manufacturer;
    std::string model;
    std::string device;
};

struct Target {
    Model model = Model::Unknown;
    const char* name = "Unknown Android device";
    int width = 0;
    int height = 0;

    bool recognized() const { return model != Model::Unknown; }
};

inline std::string lowerAscii(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

inline Target classify(const DeviceIdentity& identity) {
    const std::string manufacturer = lowerAscii(identity.manufacturer);
    const std::string model = lowerAscii(identity.model);
    const std::string device = lowerAscii(identity.device);

    if (manufacturer.find("oculus") == std::string::npos &&
        manufacturer.find("meta") == std::string::npos)
        return {};

    if (model.find("quest 3s") != std::string::npos || device == "panther")
        return {Model::Quest3S, "Meta Quest 3S", 2800, 2800};
    if (model.find("quest 3") != std::string::npos || device == "eureka")
        return {Model::Quest3, "Meta Quest 3", 2800, 2800};
    if (model.find("quest pro") != std::string::npos || device == "seacliff")
        return {Model::QuestPro, "Meta Quest Pro", 2560, 2736};
    if (model.find("quest 2") != std::string::npos || device == "hollywood")
        return {Model::Quest2, "Meta Quest 2", 2400, 2400};
    if (model == "quest" || model.find("quest 1") != std::string::npos ||
        device == "monterey")
        return {Model::Quest1, "Oculus Quest", 2048, 2272};
    return {};
}

}  // namespace questdisplay
