#include <iostream>
#include <fstream>
#include <sstream>
#include "misc.h"
#include "learn.h"
#include "uci.h"

using namespace ShashChess;

PersistedLearningUsage usePersistedLearning;
LearningData LD;

bool LearningData::load(const std::string& filename)
{
    std::ifstream in(filename, std::ios::in | std::ios::binary);

    //Quick exit if file is not present
    if (!in.is_open())
        return false;

    //Get experience file size
    in.seekg(0, std::ios::end);
    size_t fileSize = in.tellg();

    //File size should be a multiple of 'PersistedLearningMove'
    if (fileSize % sizeof(PersistedLearningMove))
    {
        std::cerr << "info string The file <" << filename << "> with size <" << fileSize << "> is not a valid experience file" << std::endl;
        return false;
    }

    //Allocate buffer to read the entire file
    void* fileData = malloc(fileSize);
    if (!fileData)
    {
        std::cerr << "info string Failed to allocate <" << fileSize << "> bytes to read experience file <" << filename << ">" << std::endl;
        return false;
    }

    //Read the entire file
    in.seekg(0, std::ios::beg); //Move read pointer to the beginning of the file
    in.read((char*)fileData, fileSize);
    if (!in)
    {
        free(fileData);

        std::cerr << "info string Failed to read <" << fileSize << "> bytes from experience file <" << filename << ">" << std::endl;
        return false;
    }

    //Close the input data file
    in.close();

    //Save pointer to fileData to be freed later
    mainDataBuffers.push_back(fileData);

    //Loop the moves from this file
    bool qLearning = (usePersistedLearning == PersistedLearningUsage::Self);
    PersistedLearningMove *persistedLearningMove = (PersistedLearningMove*)fileData;
    do
    {
        insert_or_update(persistedLearningMove, qLearning);
        ++persistedLearningMove;
    } while ((size_t)persistedLearningMove < (size_t)fileData + fileSize);

    return true;
}

void LearningData::insert_or_update(PersistedLearningMove *plm, bool qLearning)
{
    // We search in the range of all the hash table entries with key pmi
    auto range = HT.equal_range(plm->key);

    //If the 'plm' belongs to a position that did not exist before in the 'LHT'
    //then, we insert a new LearningPosition and LearningMove and return
    if (range.first == range.second)
    {
        //Insert new vector and move
        HT.insert({plm->key, &plm->learningMove });

        //Flag for persisting
        needPersisting = true;

        //Nothing else to do
        return;
    }

    //Check if this move already exists in LearningPosition
    auto itr = std::find_if(
        range.first,
        range.second,
        [&plm](const auto &p) { return p.second->move == plm->learningMove.move; });

    //If the move exists, check if it better than the move we already have
    LearningMove* bestNewMoveCandidate = nullptr;
    if (itr == range.second)
    {
        HT.insert({ plm->key, &plm->learningMove });
        bestNewMoveCandidate = &plm->learningMove;

        //Flag for persisting
        needPersisting = true;
    }
    else
    {
        LearningMove* existingMove = itr->second;
        if (existingMove->depth < plm->learningMove.depth || (existingMove->depth == plm->learningMove.depth && existingMove->score < plm->learningMove.score))
        {
            //Replace the existing move
            *existingMove = plm->learningMove;

            //Since an existing move was replaced, check the best move again
            bestNewMoveCandidate = existingMove;

            //Flag for persisting
            needPersisting = true;
        }
    }

    //Do we have a candidate for new best move?
    bool newBestMove = false;
    if (bestNewMoveCandidate != nullptr)
    {
        LearningMove *currentBestMove = range.first->second;
        if (bestNewMoveCandidate != currentBestMove)
        {
            if (qLearning)
            {
                if (bestNewMoveCandidate->score > currentBestMove->score)
                {
                    newBestMove = true;
                }
            }
            else
            {
                if ((currentBestMove->depth < bestNewMoveCandidate->depth) || (currentBestMove->depth == bestNewMoveCandidate->depth && currentBestMove->score <= bestNewMoveCandidate->score))
                {
                    newBestMove = true;
                }
            }
        }

        if (newBestMove)
        {
            //Boring and slow, but I can't think of an alternative at the moment
            //This is not thread safe, but it is fine since this code will never be called from multiuple threads
            static LearningMove lm;

            lm = *bestNewMoveCandidate;
            *bestNewMoveCandidate = *currentBestMove;
            *currentBestMove = lm;
        }
    }
}

LearningData::LearningData() : isPaused(false), needPersisting(false) {}

LearningData::~LearningData()
{
    clear();
}

void LearningData::clear()
{
    //Clear hash table
    HT.clear();

    //Release internal data buffers
    for (void* p : mainDataBuffers)
        free(p);

    //Clear internal data buffers
    mainDataBuffers.clear();

    //Release internal new moves data buffers
    for (void* p : newMovesDataBuffers)
        free(p);

    //Clear internal new moves data buffers
    newMovesDataBuffers.clear();    
}

void LearningData::init()
{
	clear();

    load(Utility::map_path("experience.bin"));

    std::vector<std::string> slaveFiles;

    //Just in case, check and load for "experience_new.bin" which will be present if
    //previous saving operation failed (engine crashed or terminated)
    std::string slaveFile = Utility::map_path("experience_new.bin");
    if (load("experience_new.bin"))
        slaveFiles.push_back(slaveFile);

    //Load slave experience files (if any)
    int i = 0;
    while (true)
    {
        slaveFile = Utility::map_path("experience" + std::to_string(i) + ".bin");
        bool loaded = load(slaveFile);
        if (!loaded)
            break;

        slaveFiles.push_back(slaveFile);
        ++i;
    }

    //We need to write all consolidated experience to disk
    if (slaveFiles.size())
        persist();

    //Remove slave files
    for (std::string fn : slaveFiles)
        remove(fn.c_str());

    //Clear the 'needPersisting' flag
    needPersisting = false;
}

void setUsePersistedLearning()
{
    if (Options["Persisted learning"] == "Off")
    {
        usePersistedLearning = PersistedLearningUsage::Off;
    }
    else if (Options["Persisted learning"] == "Standard")
    {
        usePersistedLearning = PersistedLearningUsage::Standard;
    }
    else //Classical
    {
        usePersistedLearning = PersistedLearningUsage::Self;
    }
}

void LearningData::persist()
{
    //Quick exit if we have nothing to persist
    if (HT.empty() || !needPersisting)
        return;

    /*
        To avoid any problems when saving to experience file, we will actually do the following:
        1) Save new experience to "experience_new.bin"
        2) Remove "experience.bin"
        3) Rename "experience_new.bin" to "experience.bin"

        This approach is failproof so that the old file is only removed when the new file is sufccessfully saved!
        If, for whatever odd reason, the engine is able to execute step (1) and (2) and fails to execute step (3)
        i.e., we end up with experience0.bin then it is not a problem since the file will be loaded anyway the next
        time the engine starts!
    */

    std::string experienceFilename;
    std::string tempExperienceFilename;

    if ((bool)Options["Concurrent Experience"])
    {
        static std::string uniqueStr;

        if (uniqueStr.empty())
        {
            PRNG prng(now());

            std::stringstream ss;
            ss << std::hex << prng.rand<uint64_t>();

            uniqueStr = ss.str();
        }

        experienceFilename = Utility::map_path("experience-" + uniqueStr + ".bin");
        tempExperienceFilename = Utility::map_path("experience_new-" + uniqueStr + ".bin");
    }
    else
    {
        experienceFilename = Utility::map_path("experience.bin");
        tempExperienceFilename = Utility::map_path("experience_new.bin");
    }

    std::ofstream outputFile(tempExperienceFilename, std::ofstream::trunc | std::ofstream::binary);
    PersistedLearningMove persistedLearningMove;
    for (auto& kvp : HT)
    {
        persistedLearningMove.key = kvp.first;
        persistedLearningMove.learningMove = *kvp.second;
        outputFile.write((char*)&persistedLearningMove, sizeof(persistedLearningMove));
    }
    outputFile.close();

    remove(experienceFilename.c_str());
    rename(tempExperienceFilename.c_str(), experienceFilename.c_str());

    //Prevent persisting again without modifications
    needPersisting = false;
}

void LearningData::pause()
{
    isPaused = true;
}

void LearningData::resume()
{
    isPaused = false;
}

bool LearningData::is_paused()
{
    return isPaused;
}

void LearningData::add_new_learning(Key key, const LearningMove& lm)
{
    //Allocate buffer to read the entire file
    PersistedLearningMove *newPlm = (PersistedLearningMove *)malloc(sizeof(PersistedLearningMove));
    if (!newPlm)
    {
        std::cerr << "info string Failed to allocate <" << sizeof(PersistedLearningMove) << "> bytes for new learning entry" << std::endl;
        return;
    }

    //Save pointer to fileData to be freed later
    newMovesDataBuffers.push_back(newPlm);

    //Assign
    newPlm->key = key;
    newPlm->learningMove = lm;

    //Add to HT
    insert_or_update(newPlm, (usePersistedLearning == PersistedLearningUsage::Self));
}

int LearningData::probe(Key key, const LearningMove*& learningMove)
{
    auto range = HT.equal_range(key);

    if (range.first == range.second)
    {
        learningMove = nullptr;
        return 0;
    }

    learningMove = range.first->second;
    return std::distance(range.first, range.second);
}

const LearningMove* LearningData::probe_move(Key key, Move move)
{
    auto range = HT.equal_range(key);

    if (range.first == range.second)
        return nullptr;

    auto itr = std::find_if(
        range.first,
        range.second,
        [&move](const auto &p) { return p.second->move == move; });

    if (itr == range.second)
        return nullptr;

    return itr->second;
}
