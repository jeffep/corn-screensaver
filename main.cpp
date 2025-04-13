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
#include <iomanip>
#include <cstring>

using json = nlohmann::json;

// Redirect stderr and stdout to files
void redirectOutput() {
    freopen("error.log", "w", stderr);
    freopen("output.log", "w", stdout);
    std::cerr << "Testing stderr redirection\n";
    std::cout << "Testing stdout redirection\n";
    std::cerr.flush();
    std::cout.flush();
}

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

bool refreshToken(httplib::Client& client, bool debug) {
    if (APP_KEY.empty() || APP_SECRET.empty() || REFRESH_TOKEN.empty()) {
        std::cerr << "Missing credentials\n";
        return false;
    }

    httplib::Headers headers;
    headers.emplace("Authorization", "Basic " + httplib::detail::base64_encode(APP_KEY + ":" + APP_SECRET));

    auto res = client.Post(
        "/v1/oauth/token",
        headers,
        "grant_type=refresh_token&refresh_token=" + REFRESH_TOKEN,
        "application/x-www-form-urlencoded"
    );

    if (res && res->status == 200) {
        json j = json::parse(res->body);
        ACCESS_TOKEN = j["access_token"].get<std::string>();
        REFRESH_TOKEN = j["refresh_token"].get<std::string>();
        std::ofstream env(".env");
        env << "APP_KEY=" << APP_KEY << "\n";
        env << "APP_SECRET=" << APP_SECRET << "\n";
        env << "ACCESS_TOKEN=" << ACCESS_TOKEN << "\n";
        env << "REFRESH_TOKEN=" << REFRESH_TOKEN << "\n";
        std::cout << "Token refreshed\n";
        if (debug) {
            std::ofstream debugLog("api.log", std::ios::app);
            debugLog << "Token refreshed: " << std::time(nullptr) << "\n";
            debugLog.close();
        }
        return true;
    }
    std::cerr << "Refresh failed: " << (res ? res->status : -1) << "\n";
    if (debug) {
        std::ofstream debugLog("api.log", std::ios::app);
        debugLog << "Refresh failed: " << (res ? res->status : -1) << " at " << std::time(nullptr) << "\n";
        debugLog.close();
    }
    return false;
}

double getFuturesPrice(httplib::Client& client, bool debug) {
    if (debug) {
        std::ofstream debugLog("api.log", std::ios::app);
        debugLog << "Attempting quote fetch: " << std::time(nullptr) << "\n";
        debugLog.close();
    }

    auto res = client.Get(
        "/marketdata/v1/quotes?symbols=/ZCZ25",
        httplib::Headers{{"Authorization", "Bearer " + ACCESS_TOKEN}}
    );

    if (res && res->status == 401) {
        if (debug) {
            std::ofstream debugLog("api.log", std::ios::app);
            debugLog << "Received 401, attempting refresh: " << std::time(nullptr) << "\n";
            debugLog.close();
        }
        if (refreshToken(client, debug)) {
            if (debug) {
                std::ofstream debugLog("api.log", std::ios::app);
                debugLog << "Refresh successful, retrying quote\n";
                debugLog.close();
            }
            res = client.Get(
                "/marketdata/v1/quotes?symbols=/ZCZ25",
                httplib::Headers{{"Authorization", "Bearer " + ACCESS_TOKEN}}
            );
        } else if (debug) {
            std::ofstream debugLog("api.log", std::ios::app);
            debugLog << "Refresh failed\n";
            debugLog.close();
        }
    }

    if (res && res->status == 200) {
        try {
            json j = json::parse(res->body);
            if (debug) {
                std::ofstream debugLog("api.log", std::ios::app);
                debugLog << "Raw response: " << j.dump(2) << "\n";
                debugLog.close();
            }
            if (j.contains("/ZCZ25") && j["/ZCZ25"].contains("quote")) {
                auto quote = j["/ZCZ25"]["quote"];
                double price = 0.0;
                if (quote.contains("lastPrice") && quote["lastPrice"].is_number() && quote["lastPrice"].get<double>() > 0) {
                    price = quote["lastPrice"].get<double>();
                    if (debug) {
                        std::ofstream debugLog("api.log", std::ios::app);
                        debugLog << "Using lastPrice: " << price << "\n";
                        debugLog.close();
                    }
                } else if (quote.contains("closePrice") && quote["closePrice"].is_number() && quote["closePrice"].get<double>() > 0) {
                    price = quote["closePrice"].get<double>();
                    if (debug) {
                        std::ofstream debugLog("api.log", std::ios::app);
                        debugLog << "Using closePrice: " << price << "\n";
                        debugLog.close();
                    }
                } else if (quote.contains("mark") && quote["mark"].is_number() && quote["mark"].get<double>() > 0) {
                    price = quote["mark"].get<double>();
                    if (debug) {
                        std::ofstream debugLog("api.log", std::ios::app);
                        debugLog << "Using mark: " << price << "\n";
                        debugLog.close();
                    }
                } else if (debug) {
                    std::ofstream debugLog("api.log", std::ios::app);
                    debugLog << "Error: No valid price found (lastPrice, closePrice, mark)\n";
                    debugLog.close();
                }
                if (price > 0) {
                    if (debug) {
                        std::ofstream debugLog("api.log", std::ios::app);
                        debugLog << "Success, price: " << price << "\n";
                        debugLog.close();
                    }
                    return price;
                }
            } else if (debug) {
                std::ofstream debugLog("api.log", std::ios::app);
                debugLog << "Error: Expected fields missing (/ZCZ25 or quote)\n";
                debugLog.close();
            }
        } catch (const std::exception& e) {
            if (debug) {
                std::ofstream debugLog("api.log", std::ios::app);
                debugLog << "JSON parse error: " << e.what() << "\n";
                debugLog.close();
            }
        }
    }
    if (debug) {
        std::ofstream debugLog("api.log", std::ios::app);
        debugLog << "Quote failed: " << (res ? res->status : -1) << "\n";
        debugLog.close();
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
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now_c));

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

int main(int argc, char* argv[]) {
    bool debug = false;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Corn Futures Chart\n"
                      << "Displays real-time corn futures (/ZCZ25) prices in a graphical chart.\n"
                      << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -h       Show this help message and exit\n"
                      << "  -d       Enable debug logging to api.log\n";
            return 0;
        } else if (std::strcmp(argv[i], "-d") == 0) {
            debug = true;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    redirectOutput();

    httplib::Client client("https://api.schwabapi.com");
    sqlite3* db;
    if (initDatabase(db)) return 1;

    sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
    sf::RenderWindow window(sf::VideoMode(desktop.width, desktop.height), "Corn Futures", sf::Style::Fullscreen);
    window.setFramerateLimit(60);

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
            if (event.type == sf::Event::Closed) {
                window.close();
            }
            // Exit on any touch or click
            if (event.type == sf::Event::MouseButtonPressed && event.mouseButton.button == sf::Mouse::Left) {
                sqlite3_close(db);
                window.close();
            }
            if (event.type == sf::Event::TouchBegan) {
                sqlite3_close(db);
                window.close();
            }
        }

        if (!isMarketBreak()) {
            double price = getFuturesPrice(client, debug);
            if (price > 0) {
                storePrice(db, price);
                priceHistory.push_back(price);
                if (priceHistory.size() > maxPoints) priceHistory.erase(priceHistory.begin());
                minPrice = std::max(300.0, std::min(minPrice, price - 10));
                maxPrice = std::min(600.0, std::max(maxPrice, price + 10));
                if (maxPrice - minPrice < 1.0) maxPrice = minPrice + 1.0;
            }
        }

        window.clear(sf::Color::Black);

        sf::Vector2u windowSize = window.getSize();
        float margin = 50.0f;
        float graphLeft = margin + 50.0f;
        float graphRight = windowSize.x - margin;
        float graphTop = margin;
        float graphBottom = windowSize.y - margin - 50.0f;
        float graphWidth = graphRight - graphLeft;
        float graphHeight = graphBottom - graphTop;

        // Draw axes
        sf::VertexArray axes(sf::Lines, 4);
        axes[0].position = sf::Vector2f(graphLeft, graphBottom);
        axes[0].color = sf::Color::White;
        axes[1].position = sf::Vector2f(graphRight, graphBottom);
        axes[1].color = sf::Color::White;
        axes[2].position = sf::Vector2f(graphLeft, graphBottom);
        axes[2].color = sf::Color::White;
        axes[3].position = sf::Vector2f(graphLeft, graphTop);
        axes[3].color = sf::Color::White;
        window.draw(axes);

        // Draw y-axis labels (prices)
        int numYLabels = 5;
        for (int i = 0; i <= numYLabels; ++i) {
            float price = minPrice + (maxPrice - minPrice) * i / numYLabels;
            float y = graphBottom - (i * graphHeight / numYLabels);
            sf::Text label;
            label.setFont(font);
            label.setCharacterSize(12);
            label.setFillColor(sf::Color::White);
            std::stringstream ss;
            ss << std::fixed << std::setprecision(2) << price;
            label.setString(ss.str());
            label.setPosition(graphLeft - 60, y - 10);
            window.draw(label);

            sf::VertexArray gridLine(sf::Lines, 2);
            gridLine[0].position = sf::Vector2f(graphLeft, y);
            gridLine[0].color = sf::Color(100, 100, 100, 50);
            gridLine[1].position = sf::Vector2f(graphRight, y);
            gridLine[1].color = sf::Color(100, 100, 100, 50);
            window.draw(gridLine);
        }

        // Draw x-axis labels (time)
        auto prices = loadRecentPrices(db, maxPoints);
        if (!prices.empty()) {
            int step = prices.size() / 5;
            if (step == 0) step = 1;
            for (size_t i = 0; i < prices.size(); i += step) {
                float x = graphLeft + (float)i / (maxPoints - 1) * graphWidth;
                sf::Text label;
                label.setFont(font);
                label.setCharacterSize(12);
                label.setFillColor(sf::Color::White);
                std::string time = prices[i].first.substr(11, 5);
                label.setString(time);
                label.setPosition(x - 20, graphBottom + 10);
                window.draw(label);
            }
        }

        // Draw price line
        if (!priceHistory.empty()) {
            sf::VertexArray line(sf::LineStrip, priceHistory.size());
            bool validPoint = false;
            for (size_t i = 0; i < priceHistory.size(); ++i) {
                float x = graphLeft + static_cast<float>(i) / (maxPoints - 1) * graphWidth;
                float normalized = (priceHistory[i] - minPrice) / (maxPrice - minPrice);
                normalized = std::max(0.0f, std::min(1.0f, normalized));
                float y = graphBottom - (normalized * graphHeight);
                if (std::isfinite(y)) {
                    line[i].position = sf::Vector2f(x, y);
                    line[i].color = sf::Color::Green;
                    validPoint = true;
                }
            }
            if (validPoint) {
                window.draw(line);
            }
        }

        // Draw title
        sf::Text title("Corn Futures (/ZCZ25)", font, 20);
        title.setPosition(graphLeft, margin / 2);
        title.setFillColor(sf::Color::White);
        window.draw(title);

        // Draw axis labels
        sf::Text xAxisLabel("Time", font, 16);
        xAxisLabel.setPosition(graphLeft + graphWidth / 2 - 20, graphBottom + 30);
        xAxisLabel.setFillColor(sf::Color::White);
        window.draw(xAxisLabel);

        sf::Text yAxisLabel("Price (USD)", font, 16);
        yAxisLabel.setPosition(margin / 2, graphTop + graphHeight / 2 - 20);
        yAxisLabel.setRotation(-90);
        yAxisLabel.setFillColor(sf::Color::White);
        window.draw(yAxisLabel);

        window.display();

        // Split sleep to check events more frequently
        for (int i = 0; i < 60 && window.isOpen(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            while (window.pollEvent(event)) {
                if (event.type == sf::Event::Closed) {
                    window.close();
                }
                if (event.type == sf::Event::MouseButtonPressed && event.mouseButton.button == sf::Mouse::Left) {
                    sqlite3_close(db);
                    window.close();
                }
                if (event.type == sf::Event::TouchBegan) {
                    sqlite3_close(db);
                    window.close();
                }
            }
        }
    }

    sqlite3_close(db);
    return 0;
}
