#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/Mod.hpp>
#include <filesystem> 

#include <cmath>
#include <random>
std::random_device rd;
std::mt19937 rng(rd());

static const int s_profaneSfx[] = { 4, 9, 10, 11, 24, 27 };
static int s_killerObjectId = -1;

using namespace geode::prelude;

void playSfx(int sfxId, float volume, bool censored) {
    if (sfxId == -1) {
        return;
    }
    std::string filename = std::to_string(sfxId) + ".ogg";
    if (censored) {
        bool contains = false;
        for (int i = 0; i < std::size(s_profaneSfx); i++) {
            if (s_profaneSfx[i] == sfxId) {
                contains = true;
                break;
            }
        }
        if (contains) {
            filename = std::to_string(sfxId) + "_c.ogg";
        }
    }
    auto path = Mod::get()->getResourcesDir() / filename;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            log::error("Error checking file: {} - {}", string::pathToString(path), ec.message());
        }
        else {
            log::error("File not found: {}", string::pathToString(path));
        }
        return;
    }

    FMOD::Sound* sound;
    FMOD::Channel* channel;
    auto engine = FMODAudioEngine::sharedEngine();

    if (engine->m_system->createSound(string::pathToString(path).c_str(), FMOD_DEFAULT, nullptr, &sound) == FMOD_OK) {
        if (engine->m_system->playSound(sound, nullptr, false, &channel) == FMOD_OK) {
            channel->setVolume(volume);
        }
    }
    else {
        log::error("Failed to create sound: {}", filename);
    }
}

class $modify(MyLevelCompleteHook, PlayLayer) {
    struct Fields {
        int scheduledSoundId;
        float scheduledVolume;
    };
    void levelComplete() {
        PlayLayer::levelComplete();

        bool enabled = Mod::get()->getSettingValue<bool>("enable") && Mod::get()->getSettingValue<bool>("levelcomplete");
        bool disabledInPractice = Mod::get()->getSettingValue<bool>("disableinpractice");

        if (!enabled) {
            return;
        }

        auto playLayer = PlayLayer::get();
        if (playLayer && disabledInPractice) {
            bool isPracticeMode = playLayer->m_isPracticeMode;
            bool isStartPos = playLayer->m_isTestMode;
            if (isPracticeMode || isStartPos) {
                return;
            }
        }

        m_fields->scheduledVolume = Mod::get()->getSettingValue<int64_t>("volume") / 100.0f;
        m_fields->scheduledSoundId = 30;
        this->scheduleOnce(schedule_selector(MyLevelCompleteHook::playMySound), 0.f);
    }
    void playMySound(float dt) {
        playSfx(m_fields->scheduledSoundId, m_fields->scheduledVolume, false);
    }
};

class $modify(MyGameObjHook, PlayLayer) {
    struct Fields {
        int killerObjectId;
    };

public:
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object) {
            s_killerObjectId = object->m_objectID;
        }
        else {
            s_killerObjectId = -1;
        }
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(MyDeathHook, PlayerObject) {
    struct Fields {
        int scheduledSoundId;
        float scheduledVolume;
        bool scheduledIsCensored;
    };
    
    public:
        
        void playDeathEffect() {
            PlayerObject::playDeathEffect();

            bool enabled = Mod::get()->getSettingValue<bool>("enable");
            bool disabledInPractice = Mod::get()->getSettingValue<bool>("disableinpractice");
            bool disabledInPlat = Mod::get()->getSettingValue<bool>("disableinplat");
            
            if (!enabled) {
                return;
            }

            auto playLayer = PlayLayer::get();
            if (playLayer) {
                if (disabledInPractice) {
                    bool isPracticeMode = playLayer->m_isPracticeMode;
                    bool isStartPos = playLayer->m_isTestMode;
                    if (isPracticeMode || isStartPos) {
                        return;
                    }
                }
                if (disabledInPlat && playLayer->m_level->isPlatformer()) {
                    return;
                }
            }

            int currentBest = playLayer ? playLayer->m_level->m_normalPercent : 0;
            int deathPercent = playLayer ? static_cast<int>(playLayer->getCurrentPercent()) : 0;
            bool waveDeath = this->m_isDart;
                        
            m_fields->scheduledVolume = Mod::get()->getSettingValue<int64_t>("volume") / 100.0f;
            m_fields->scheduledSoundId = chooseDeathSound(deathPercent, currentBest, waveDeath, s_killerObjectId);
            m_fields->scheduledIsCensored = Mod::get()->getSettingValue<std::string>("censored") == "Censored";

            this->scheduleOnce(schedule_selector(MyDeathHook::playMySound), 0.f);
        }
        
        void playMySound(float dt) {
            playSfx(m_fields->scheduledSoundId, m_fields->scheduledVolume, m_fields->scheduledIsCensored);
        }

        bool isSpikeObject(int objectId) {
            // i got this list from manually checking Colon's level in-game, not sure if its 100% inclusive but should be mostly accurate
            static const std::unordered_set<int> spikeIds = { 8, 39, 103, 144, 145, 177, 178, 179, 191, 198, 199, 205, 216, 217, 218, 392, 393, 458, 459, 667, 1703, 1889, 1890, 1891, 1892 };
            return spikeIds.count(objectId) > 0;
        }

        int chooseDeathSound(int deathPercent, int currentBest, bool waveDeath, int killerObjectId) {

            auto censored = Mod::get()->getSettingValue<std::string>("censored");
            auto customization = Mod::get()->getSettingValue<std::string>("customize_deaths");
            auto decentThreshold = Mod::get()->getSettingValue<int64_t>("decent_threshold");
            auto farThreshold = Mod::get()->getSettingValue<int64_t>("far_threshold");
            bool alwaysPlayOnBest = Mod::get()->getSettingValue<bool>("alwaysbest");

            int rageSize =
                // small rage: 0% - decentThreshold (regardless of current best)
                // current best rage: 3% up to and including current best
                // medium rage: minimum decentThreshold, maximum farThreshold
                // large rage: minimum farThreshold
                (deathPercent < decentThreshold) ? 1 :
                (deathPercent >= decentThreshold && std::abs(deathPercent - currentBest) <= 2) ? 3 :
                (deathPercent >= decentThreshold && deathPercent < farThreshold) ? 2 :
                4;

            if (customization == "Decent attempts" && deathPercent <= decentThreshold) {
                if (!(alwaysPlayOnBest && deathPercent > currentBest)) {
                    return -1;
                }
            }
            if (customization == "Far attempts" && deathPercent <= farThreshold) {
                if (!(alwaysPlayOnBest && deathPercent > currentBest)) {
                    return -1;
                }
            }
           
            static const int smallRageSfx[] = { 2, 3, 5, 26 };
            static const int mediumRageSfx[] = { 8, 9, 11, 24, 25, 27, 28, 29 };
            static const int largeRageSfx[] = { 1, 4, 6, 12, 13, 15, 17, 20, 21, 22, 23 };
            static const int currentBestRageSfx[] = { 7, 16, 18 };
            static const int spikeDeathRageSfx[] = { 10, 19 };
            static const int waveDeathRageSfx[] = { 14 };

            std::vector<int> pool;

            auto add = [&](auto& arr) {
                using T = std::remove_reference_t<decltype(arr)>;
                pool.insert(pool.end(), std::begin(arr), std::end(arr));
            };
            auto removeAll = [&](auto& arr) {
                using T = std::remove_reference_t<decltype(arr)>;
                for (auto& val : arr) {
                    pool.erase(
                        std::remove(pool.begin(), pool.end(), val),
                        pool.end()
                    );
                }
            };
           
            switch (rageSize) {
                case 1: add(smallRageSfx); break;
                case 2: add(mediumRageSfx); break;
                case 3: add(currentBestRageSfx); break;
                case 4: add(largeRageSfx); break;
            }

            if (isSpikeObject(killerObjectId) && rageSize >= 2) {
                add(spikeDeathRageSfx);
            }
            if (waveDeath && rageSize >= 2) {
                add(waveDeathRageSfx);
            }
            if (censored == "Disabled") {
                removeAll(s_profaneSfx);
            }

            std::uniform_int_distribution<> dis(0, pool.size() - 1);
            return pool[dis(rng)];
        }
};