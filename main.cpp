#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <SFML/Graphics.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <ctime>

using json = nlohmann::json;

// Load credentials from .env
std::string loadEnv(const std::string& key) {
    std::ifstream file(".env");
    std::string line, value;
    while (std::getline(file, line)) {
        if (line.find(key + "=") == 0) {
            value = line.substr(key.length() + 1);
            break;
        }
    }
    return value;
}

std::string APP_KEY = loadEnv("APP_KEY");
std::string APP_SECRET = loadEnv("APP_SECRET");
std::string ACCESS_TOKEN = loadEnv("ACCESS_TOKEN");
std::string REFRESH_TOKEN = loadEnv("REFRESH_TOKEN");

bool refreshToken(httplib::Client& client) {
    if (APP_KEY.empty() || APP_SECRET.empty() || REFRESH_TOKEN.empty()) {
        std::cerr << "Missing credentials\n";
        return false;
    }
    auto res = client.Post(
        "/v1/oauth/token",
        "grant_type=refresh_token&refresh_token=" + REFRESH_TOKEN,
        httplib::Headers{{"Authorization", "Basic " + httplib::base64_encode(APP_KEY + ":" + APP_SECRET)}},
        "application/x-www-form-urlencoded"
    );

    if (res && res->status == 200) {
        json j = json::parse(res->body);
        ACCESS_TOKEN = j["access_token"].get<std::string>();
        REFRESH_TOKEN = j["refresh_token"].get<std::string>();
        // Update .env
        std::ofstream env(".env");
        env << "APP_KEY=" << APP_KEY << "\n";
        env << "APP_SECRET=" << APP_SECRET << "\n";
        env << "ACCESS_TOKEN=" << ACCESS_TOKEN << "\n";
        env << "REFRESH_TOKEN=" << REFRESH_TOKEN << "\n";
        std::cout << "Token refreshed\n";
        return true;
    }
    std::cerr << "Refresh failed: " << (res ? res->status : -1) << "\n";
    return false;
}

double getFuturesPrice(httplib::Client& client) {
    auto res = client.Get(
        "/marketdata/v1/quotes?symbols=/ZCZ25",
        httplib::Headers{{"Authorization", "Bearer " + ACCESS_TOKEN}}
    );

    if (res && res->status == 401) {
        if (refreshToken(client)) {
            res = client.Get(
                "/marketdata/v1/quotes?symbols=/ZCZ25",
                httplib::Headers{{"Authorization", "Bearer " + ACCESS_TOKEN}}
            );
        }
    }

    if (res && res->status == 200) {
        json j = json::parse(res->body);
        return j["/ZCZ25"]["quote"]["lastPrice"].get<double>();
    }
    std::cerr << "Quote failed: " << (res ? res->status : -1) << "\n";
    return -1.0;
}

int initDatabase(sqlite3*& db) {
    if (sqlite3_open("corn.db", &db) != SQLITE_OK) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << "\n";
        return 1;
    }
    const char* sql = "CREATE TABLE IF NOT EXISTS prices ("
                      "timestamp TEXT NOT NULL,"
                      "price REAL NOT NULL);";
    char* errMsg = 0;
    if (sqlite3_exec(db, sql, 0, 0, &errMsg) != SQLITE_OK) {
        std::cerr << "SQL error: " << errMsg << "\n";
        sqlite3_free(errMsg);
        sqlite3_close(db);
        return 1;
    }
    return 0;
}

int storePrice(sqlite3* db, double price) {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    char timestamp[20];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%:M:%S", std::localtime(&now_c));

    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO prices (timestamp, price) VALUES (?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
        std::cerr << "Prepare failed: " << sqlite3_errmsg(db) << "\n";
        return 1;
    }
    sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, price);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Step failed: " << sqlite3_errmsg(db) << "\n";
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

std::vector<std::pair<std::string, double>> loadRecentPrices(sqlite3* db, int limit = 60) {
    std::vector<std::pair<std::string, double>> prices;
    const char* sql = "SELECT timestamp, price FROM prices ORDER BY timestamp DESC LIMIT ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
        std::cerr << "Prepare failed: " << sqlite3_errmsg(db) << "\n";
        return prices;
    }
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        double price = sqlite3_column_double(stmt, 1);
        prices.emplace_back(ts, price);
    }
    sqlite3_finalize(stmt);
    std::reverse(prices.begin(), prices.end());
    return prices;
}

bool isMarketBreak() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&now_c);
    int hour = tm->tm_hour;
    int min = tm->tm_min;
    int wday = tm->tm_wday;
    return wday >= 1 && wday <= 5 && hour == 15 && min >= 0 && min < 60;
}

int main() {
    httplib::Client client("https://api.schwabapi.com");
    sqlite3* db;
    if (initDatabase(db)) return 1;

    sf::RenderWindow window(sf::VideoMode(800, 600), "Corn Futures");
    sf::Font font;
    if (!font.loadFromFile("arial.ttf")) {
        std::cerr << "Font load failed\n";
        sqlite3_close(db);
        return 1;
    }

    std::vector<double> priceHistory;
    const int maxPoints = 60;
    double minPrice = 400, maxPrice = 500;

    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();
        }

        if (!isMarketBreak()) {
            double price = getFuturesPrice(client);
            if (price > 0) {
                storePrice(db, price);
                priceHistory.push_back(price);
                if (priceHistory.size() > maxPoints) priceHistory.erase(priceHistory.begin());
                minPrice = std::min(minPrice, price - 10);
                maxPrice = std::max(maxPrice, price + 10);
            }
        }

        window.clear();
        if (!priceHistory.empty()) {
            sf::VertexArray line(sf::LineStrip, priceHistory.size());
            for (size_t i = 0; i < priceHistory.size(); ++i) {
                float x = static_cast<float>(i) / (maxPoints - 1) * 800;
                float y = 600 - ((priceHistory[i] - minPrice) / (maxPrice - minPrice) * 550 + 25);
                line[i].position = sf::Vector2f(x, y);
                line[i].color = sf::Color::Green;
            }
            window.draw(line);
        }

        sf::Text text("Corn Futures (/ZCZ25)", font, 20);
        text.setPosition(10, 10);
        text.setFillColor(sf::Color::White);
        window.draw(text);

        window.display();
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }

    sqlite3_close(db);
    return 0;
}
