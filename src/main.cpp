#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/Mod.hpp>
#include <filesystem> 

#include <random>
std::random_device rd;
std::mt19937 rng(rd());

using namespace geode::prelude;

void playSfx(int sfxId, float volume) {
    if (sfxId == -1) {
        return;
    }
    std::string filename = std::to_string(sfxId) + ".ogg";
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
        playSfx(m_fields->scheduledSoundId, m_fields->scheduledVolume);
    }
};

class $modify(MyDeathHook, PlayerObject) {
    struct Fields {
        int scheduledSoundId;
        float scheduledVolume;
    };
    
    public:
        void playDeathEffect() {
            PlayerObject::playDeathEffect();

            bool enabled = Mod::get()->getSettingValue<bool>("enable");
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

            int currentBest = playLayer ? playLayer->m_level->m_normalPercent : 0;
            int deathPercent = playLayer ? static_cast<int>(playLayer->getCurrentPercent()) : 0;
            bool waveDeath = this->m_isDart;
                        
            m_fields->scheduledVolume = Mod::get()->getSettingValue<int64_t>("volume") / 100.0f;
            m_fields->scheduledSoundId = chooseDeathSound(deathPercent, currentBest, waveDeath);
            this->scheduleOnce(schedule_selector(MyDeathHook::playMySound), 0.f);
        }
        
        void playMySound(float dt) {
            playSfx(m_fields->scheduledSoundId, m_fields->scheduledVolume);
        }

        int chooseDeathSound(int deathPercent, int currentBest, bool waveDeath) {
            bool playOnBestOnly = Mod::get()->getSettingValue<bool>("bestonly");
            int rageSize =
                // small rage: 0% - 25% (regardless of current best)
                // medium rage: minimum 25%, maximum is the smaller of either 85% or 5% before current best
                // current best rage: 5% up to and including current best (or current best only if considerSmallMedRage is false)
                // large rage: past the best
                (deathPercent < 25) ? 1 :
                (deathPercent + 5 <= currentBest && deathPercent < 85) ? 2 :
                (deathPercent <= currentBest && deathPercent < 85) ? 3 :
                4;
            if (playOnBestOnly && deathPercent < currentBest) {
                return -1;
            }
           
            static const int smallRageSfx[] = { 2, 3, 5, 26 };
            static const int mediumRageSfx[] = { 8, 9, 11, 24, 25, 27, 28, 29 };
            static const int largeRageSfx[] = { 1, 4, 6, 12, 13, 15, 17, 20, 21, 22, 23 };
            static const int currentBestRageSfx[] = { 7, 16, 18 };
            static const int spikeDeathRageSfx[] = { 10, 19 };
            static const int waveDeathRageSfx[] = { 14, 14};

            std::vector<int> pool;

            auto add = [&](auto& arr) {
                using T = std::remove_reference_t<decltype(arr)>;
                pool.insert(pool.end(), std::begin(arr), std::end(arr));
            };
           
            switch (rageSize) {
                case 1: add(smallRageSfx); break;
                case 2: add(mediumRageSfx); break;
                case 3: add(currentBestRageSfx); break;
                case 4: add(largeRageSfx); break;
            }

            if (rageSize == 4) {
                // was gonna make this only activate when the player actually dies to a spike
                // but cant figure out how to do that (this is my first geode mod lol)
                // feel free to make a pr if you are more experienced than me
                add(spikeDeathRageSfx);
            }
            if (waveDeath && rageSize >= 2) {
                add(waveDeathRageSfx);
            }

            std::uniform_int_distribution<> dis(0, pool.size() - 1);
            return pool[dis(rng)];
        }
};