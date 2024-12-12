#include "LSH_Wrapper.h"
#include "LSH.h"
#include "ReadFile.h"
#include "NGram.h"
#include "Memory_Usage.h"
#include "util.h"
#include <chrono>
#include <unordered_set>
#include <future>
#include <cmath>
#include <tbb/concurrent_unordered_map.h>

std::unordered_set<std::string> word_set = {"about", "all", "any", "as", "but", "can",
                                            "choice", "extra", "for", "free", "from", "good", "i", "if", "in", "inch",
                                            "into", "is", "like", "more", "none", "not", "of", "on", "one",
                                            "optional", "other", "pieces", "plus", "possibly", "removed", "size", "such",
                                            "the", "to", "up", "use", "very", "weight", "with", "you", "your"};
std::queue<std::pair<std::string, std::vector<std::string>>> tasks;
tbb::concurrent_unordered_map<std::string, std::string> inverted_index;
tbb::concurrent_unordered_map<std::string, std::unordered_set<std::string>> cache;
tbb::concurrent_unordered_map<std::string, std::unordered_set<std::string>> cache_single;
tbb::concurrent_unordered_map<std::string, std::unordered_set<std::string>> mismatch;
tbb::concurrent_unordered_map<std::string, std::unordered_set<std::string>> ingredients_matches;
tbb::concurrent_unordered_map<std::string, std::unordered_set<std::string>> inverted_index_multiple;
tbb::concurrent_unordered_map<std::string, std::unordered_set<std::string>> inverted_index_single;
std::unordered_map<std::string, std::unordered_set<std::string>> matches;
std::mutex mutex;

std::vector<std::string> filter_string(const std::string& input_string) {
    std::istringstream iss(input_string);
    std::vector<std::string> words;
    std::string word;

    std::string filtered_word;

    while (iss >> word) {
        filtered_word.clear();
        for (char ch : word) {
            if (std::isalpha(ch) || std::isspace(ch)) {
                filtered_word += std::tolower(ch);
            }
        }
        if (filtered_word.empty()) {
            continue;
        }
        if (word_set.find(filtered_word) == word_set.end()) {
            words.push_back(std::move(filtered_word));
            filtered_word = std::string();
        }
    }
    return words;
}

void process_chunk_words(
    const std::unordered_map<std::string, std::string>::iterator& start,
    const std::unordered_map<std::string, std::string>::iterator& end, int thread_id) {

    std::unordered_map<std::string, std::unordered_set<std::string>> local_index_multiple;
    std::unordered_map<std::string, std::unordered_set<std::string>> local_index_single;

    int completed_tasks = 0;

    for (auto it = start; it != end; ++it) {
        auto filtered_string = filter_string(it->second);
        auto words = text_to_ngrams_words(filtered_string, 2);

        for (const auto& w : words) {
            local_index_multiple[w].insert(it->first);
        }

        auto single_words = text_to_ngrams_words(filtered_string, 1);
        for (const auto& w : single_words) {
            local_index_single[w].insert(it->first);
        }
        completed_tasks++;
        if (completed_tasks % 10000 == 0) {
            std::cout << completed_tasks << "th task completed on thread " << thread_id << std::endl;
        }
    }

    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [key, value] : local_index_multiple) {
        inverted_index_multiple[key].insert(value.begin(), value.end());
    }
    local_index_multiple.clear();
    for (const auto& [key, value] : local_index_single) {
        inverted_index_single[key].insert(value.begin(), value.end());
    }
    local_index_single.clear();
}

void process_chunk(int thread_id, const std::vector<std::pair<std::string, std::string>>& tasks,
                   LSH& lsh, int n){
    int completedTasks = 0;
    std::unordered_map<std::string, std::unordered_set<std::string>> local_mismatch;
    std::unordered_map<std::string, std::unordered_set<std::string>> local_ingredients_matches;
    for (int i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        auto key = std::get<0>(task);
        std::unordered_set<std::string> set;
        auto indicator = std::get<1>(task);
        if (indicator == "single") {
            auto candidates = lsh.query(text_to_ngrams(key, n), 0.9);
            set.insert(candidates.begin(), candidates.end());
        }
        else {
            auto candidates = lsh.query(text_to_ngrams(key, n), 0.5);
            set.insert(candidates.begin(), candidates.end());
        }

        local_ingredients_matches[key].insert(set.begin(), set.end());

        completedTasks++;
    }

    for (auto& pair : local_ingredients_matches) {
        ingredients_matches[pair.first].insert(pair.second.begin(), pair.second.end());
    }
}

void match(std::string ontologyPath, std::string ingredientPath, std::string outputPath, int hash_funcs = 100, int band = 25) {
    LSH lsh(band, hash_funcs);
    std::string filename = outputPath;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index;
    int n = 3;
    tbb::concurrent_unordered_map<std::string, std::unordered_set<std::string>> cache;

    json json = process_json(ontologyPath);
    std::unordered_map<std::string, std::vector<std::string>> lexMaprIngredients = processCSV(ingredientPath, 1);
    std::unordered_map<std::string, std::unordered_set<std::string>> possible_matches;
    std::unordered_map<std::string, std::string> ingredients;
    for (auto& [key, value] : lexMaprIngredients) {
        ingredients[key] = value[0];
        possible_matches[key].insert(value.begin() + 1, value.end());
    }
    lexMaprIngredients.clear();
    std::vector<std::string> ontologies = parseJson(json, index, inverted_index);

    const size_t max_concurrent_tasks = std::min(std::thread::hardware_concurrency(), static_cast<unsigned int>(ingredients.size()));
    std::vector<std::thread> threads;

    auto it = ingredients.begin();
    size_t length = ingredients.size() / max_concurrent_tasks;

    for (size_t i = 0; i < max_concurrent_tasks; ++i) {
        auto end = std::next(it, i == max_concurrent_tasks - 1 ? ingredients.size() - length * i : length);
        threads.emplace_back(process_chunk_words, it, end, i);
        it = end;
    }

    for (auto& t : threads) {
        t.join();
    }

    std::string bin_filename = get_base_filename(ontologyPath) + ".bin";
    if (file_exists(bin_filename)) {
        lsh.load_from_disk(bin_filename);
    }
    else {
        for (int i = 0; i < ontologies.size(); ++i) {
            lsh.insert(text_to_ngrams(ontologies[i], n), ontologies[i]);
        }
        lsh.save_to_disk(bin_filename);
    }
    
    std::vector<std::pair<std::string, std::string>> tasks;
    
    for (auto& [key, value] : inverted_index_multiple) {
        tasks.push_back({key, "multiple"});
    }

    for (auto& [key, value] : inverted_index_single) {
        tasks.push_back({key, "single"});
    }

    size_t chunk_size = (tasks.size() + max_concurrent_tasks - 1) / max_concurrent_tasks;
    std::vector<std::thread> workers;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < max_concurrent_tasks; ++i) {
        size_t start = i * chunk_size;
        size_t end = std::min(start + chunk_size, tasks.size());
        std::vector<std::pair<std::string, std::string>> chunk_tasks(tasks.begin() + start, tasks.begin() + end);

        workers.emplace_back(process_chunk, i, chunk_tasks,
                            std::ref(lsh), n);
    }

    for (auto& worker : workers) {
        worker.join();
    }
    
    auto stop_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(stop_time - start_time);
    int total_seconds = duration.count();
    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;
    
    std::ofstream outFile(filename);

    if (!outFile.is_open()) {
        std::cerr << "Failed to open " << filename << std::endl;
        return;
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> matches;
    for (auto& [key, value] : ingredients_matches) {
        if (inverted_index_multiple.find(key) != inverted_index_multiple.end()) {
            auto lst = inverted_index_multiple[key];
            for (auto& element : lst) {
                matches[element].insert(value.begin(), value.end());
            }
        }
        else if (inverted_index_single.find(key) != inverted_index_single.end()) {
            auto lst = inverted_index_single[key];
            for (auto& element : lst) {
                matches[element].insert(value.begin(), value.end());
            }
        }
    }

    for (auto& [key, value] : matches) {
        outFile << key << std::endl;
        for (auto& v : value) {
            outFile << "(" << index[v].first << " " << index[v].second << "), ";
        }
        outFile << "\n";
    }

}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cout << "Usage: ./EntityMatching [path_to_ontology] [path_to_candiates] [path_to_output]\n";
        return -1;
    }
    match(argv[1], argv[2], argv[3]);
}