#include "database/person_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>
#include <nlohmann/json.hpp>
#include <ctime>
#include <cmath>
#include <algorithm>

namespace vms {
namespace database {

std::vector<Person> PersonRepository::getAllPersons() {
    std::vector<Person> persons;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, name, description, face_image_path, embedding_json, created_at, updated_at FROM persons ORDER BY id DESC";

    QSqlQuery query(db);
    query.prepare(sql);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getAllPersons: {}", query.lastError().text().toStdString());
        return persons;
    }

    while (query.next()) {
        Person person;
        person.id = query.value(0).toInt();
        person.name = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        person.description = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        person.face_image_path = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        person.embedding_json = query.value(4).isNull() ? "[]" : query.value(4).toString().toStdString();
        person.created_at = query.value(5).toLongLong();
        person.updated_at = query.value(6).toLongLong();
        persons.push_back(person);
    }

    return persons;
}

std::optional<Person> PersonRepository::getPersonById(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, name, description, face_image_path, embedding_json, created_at, updated_at FROM persons WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getPersonById: {}", query.lastError().text().toStdString());
        return std::nullopt;
    }

    if (query.next()) {
        Person person;
        person.id = query.value(0).toInt();
        person.name = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        person.description = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        person.face_image_path = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        person.embedding_json = query.value(4).isNull() ? "[]" : query.value(4).toString().toStdString();
        person.created_at = query.value(5).toLongLong();
        person.updated_at = query.value(6).toLongLong();
        return person;
    }

    return std::nullopt;
}

int PersonRepository::insertPerson(const Person& person) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "INSERT INTO persons (name, description, face_image_path, embedding_json, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)";

    QSqlQuery query(db);
    query.prepare(sql);

    std::time_t now = std::time(nullptr);

    query.bindValue(0, QString::fromStdString(person.name));
    query.bindValue(1, QString::fromStdString(person.description));
    query.bindValue(2, QString::fromStdString(person.face_image_path));
    query.bindValue(3, QString::fromStdString(person.embedding_json));
    query.bindValue(4, static_cast<qint64>(now));
    query.bindValue(5, static_cast<qint64>(now));

    if (!query.exec()) {
        LOG_ERROR("Failed to insert person: {}", query.lastError().text().toStdString());
        return -1;
    }

    int inserted_id = query.lastInsertId().toInt();
    LOG_INFO("Person inserted: {} (ID: {})", person.name, inserted_id);
    return inserted_id;
}

bool PersonRepository::updatePerson(const Person& person) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "UPDATE persons SET name = ?, description = ?, face_image_path = ?, embedding_json = ?, updated_at = ? WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);

    std::time_t now = std::time(nullptr);

    query.bindValue(0, QString::fromStdString(person.name));
    query.bindValue(1, QString::fromStdString(person.description));
    query.bindValue(2, QString::fromStdString(person.face_image_path));
    query.bindValue(3, QString::fromStdString(person.embedding_json));
    query.bindValue(4, static_cast<qint64>(now));
    query.bindValue(5, person.id);

    if (!query.exec()) {
        LOG_ERROR("Failed to update person: {}", query.lastError().text().toStdString());
        return false;
    }

    LOG_INFO("Person updated: {} (ID: {})", person.name, person.id);
    return true;
}

bool PersonRepository::deletePerson(int id) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "DELETE FROM persons WHERE id = ?";

    QSqlQuery query(db);
    query.prepare(sql);
    query.bindValue(0, id);

    if (!query.exec()) {
        LOG_ERROR("Failed to delete person: {}", query.lastError().text().toStdString());
        return false;
    }

    LOG_INFO("Person deleted: ID {}", id);
    return true;
}

std::vector<Person> PersonRepository::searchByName(const std::string& name) {
    std::vector<Person> persons;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    QString sql = "SELECT id, name, description, face_image_path, embedding_json, created_at, updated_at FROM persons WHERE name LIKE ? ORDER BY id";

    QSqlQuery query(db);
    query.prepare(sql);
    QString search_pattern = "%" + QString::fromStdString(name) + "%";
    query.bindValue(0, search_pattern);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute searchByName: {}", query.lastError().text().toStdString());
        return persons;
    }

    while (query.next()) {
        Person person;
        person.id = query.value(0).toInt();
        person.name = query.value(1).isNull() ? "" : query.value(1).toString().toStdString();
        person.description = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        person.face_image_path = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        person.embedding_json = query.value(4).isNull() ? "[]" : query.value(4).toString().toStdString();
        person.created_at = query.value(5).toLongLong();
        person.updated_at = query.value(6).toLongLong();
        persons.push_back(person);
    }

    return persons;
}

std::vector<std::pair<Person, float>> PersonRepository::searchByEmbedding(const std::vector<float>& query_embedding, float threshold) {
    // 1. Get all persons
    auto all_persons = getAllPersons();
    std::vector<std::pair<Person, float>> matches;

    // Accept both 128-d (ArcFace) and 512-d (FaceNet) embeddings
    if (query_embedding.empty()) return matches;
    const size_t expected_dim = query_embedding.size();

    // Helper: Cosine Similarity
    auto cosineSim = [](const std::vector<float>& a, const std::vector<float>& b) -> float {
        if (a.size() != b.size() || a.empty()) return 0.0f;
        float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
            norm_a += a[i] * a[i];
            norm_b += b[i] * b[i];
        }
        if (norm_a <= 0 || norm_b <= 0) return 0.0f;
        return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
    };

    // 2. Compute similarity
    for (const auto& person : all_persons) {
        try {
            auto j = nlohmann::json::parse(person.embedding_json);
            std::vector<float> db_emb = j.get<std::vector<float>>();

            if (db_emb.size() == expected_dim) {
                float sim = cosineSim(query_embedding, db_emb);
                if (sim >= threshold) {
                    matches.push_back({person, sim});
                }
            }
        } catch (...) {
            continue;
        }
    }

    // 3. Sort by similarity desc
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.second > b.second; // Descending
    });

    return matches;
}

PersonRepository::PersonStats PersonRepository::getPersonStats() {
    PersonStats stats;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();

    // Total count
    {
        QString sql = "SELECT COUNT(*) FROM persons";
        QSqlQuery query(db);
        query.prepare(sql);
        if (query.exec() && query.next()) {
            stats.total = query.value(0).toInt();
        }
    }

    // With embedding (non-empty, non-"[]")
    {
        QString sql = "SELECT COUNT(*) FROM persons WHERE embedding_json IS NOT NULL AND embedding_json != '' AND embedding_json != '[]'";
        QSqlQuery query(db);
        query.prepare(sql);
        if (query.exec() && query.next()) {
            stats.with_embedding = query.value(0).toInt();
        }
    }

    // With image
    {
        QString sql = "SELECT COUNT(*) FROM persons WHERE face_image_path IS NOT NULL AND face_image_path != ''";
        QSqlQuery query(db);
        query.prepare(sql);
        if (query.exec() && query.next()) {
            stats.with_image = query.value(0).toInt();
        }
    }

    return stats;
}

} // namespace database
} // namespace vms
