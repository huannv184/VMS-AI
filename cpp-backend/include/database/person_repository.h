#pragma once

#include "models.h"
#include <vector>
#include <optional>
#include <string>

namespace vms {
namespace database {

/**
 * @brief Repository for managing Person entities in database
 */
class PersonRepository {
public:
    PersonRepository() = default;
    ~PersonRepository() = default;

    /**
     * @brief Get all registered persons
     */
    std::vector<Person> getAllPersons();

    /**
     * @brief Get person by ID
     */
    std::optional<Person> getPersonById(int id);

    /**
     * @brief Insert new person
     * @return Inserted person ID on success, -1 on failure
     */
    int insertPerson(const Person& person);

    /**
     * @brief Update person
     */
    bool updatePerson(const Person& person);

    /**
     * @brief Delete person
     */
    bool deletePerson(int id);

    /**
     * @brief Search person by name
     */
    std::vector<Person> searchByName(const std::string& name);

    /**
     * @brief Search persons by vector embedding (Cosine Similarity)
     * @param query_embedding Face embedding vector
     * @param threshold Similarity threshold (0.0 - 1.0)
     * @return List of persons with similarity scores, sorted descending
     */
    std::vector<std::pair<Person, float>> searchByEmbedding(const std::vector<float>& query_embedding, float threshold = 0.5f);

    /**
     * @brief Get total person count and embedding statistics
     */
    struct PersonStats {
        int total = 0;
        int with_embedding = 0;
        int with_image = 0;
    };
    PersonStats getPersonStats();
};

} // namespace database
} // namespace vms
