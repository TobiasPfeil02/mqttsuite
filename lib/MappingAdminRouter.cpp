/*
 * MQTTSuite - A lightweight MQTT Integration System
 * Copyright (C) Tobias Pfeil
 *               2025, 2026
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "MappingAdminRouter.h"

#include "ConfigApplication.h"
#include "JsonMappingReader.h"
#include "MqttMapper.h"

#include <express/middleware/BasicAuthentication.h>
#include <express/middleware/JsonMiddleware.h>
#include <express/middleware/StaticMiddleware.h>

#ifndef DOXYGEN_SHOULD_SKIP_THIS

#include "nlohmann/json-schema.hpp"

#include <exception>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <memory>
#include <sstream>
#include <utility>

// IWYU pragma: no_include <nlohmann/detail/json_ref.hpp>

#endif // DOXYGEN_SHOULD_SKIP_THIS

namespace {

    constexpr const char* DEFAULT_DRAFT_ID = "default";

    struct MappingApplyResult {
        std::uint64_t revision{0};
        std::string draftId;
        bool mappingPersisted{false};
        mqtt::lib::admin::ReloadResult reloadResult;
    };

    struct ActiveState {
        nlohmann::json mapping;
        std::uint64_t revision{0};
    };

    struct AdminMappingContext {
        std::string mappingFilePath;
    };

    std::optional<std::uint64_t> parseRevisionToken(const std::string& rawValue) {
        if (rawValue.empty()) {
            return std::nullopt;
        }

        std::string value = rawValue;

        if (value.starts_with("W/")) {
            value = value.substr(2);
        }

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (value.starts_with("rev-")) {
            value = value.substr(4);
        }

        if (value.empty()) {
            return std::nullopt;
        }

        try {
            std::size_t pos = 0;
            const auto parsed = std::stoull(value, &pos);
            if (pos != value.size()) {
                return std::nullopt;
            }

            return static_cast<std::uint64_t>(parsed);
        } catch (...) {
            return std::nullopt;
        }
    }

    template <typename RequestPtr>
    std::optional<std::uint64_t> resolveExpectedRevision(const RequestPtr& req, const nlohmann::json* body) {
        if (body != nullptr && body->is_object() && body->contains("expected_revision")) {
            try {
                return (*body).at("expected_revision").get<std::uint64_t>();
            } catch (...) {
            }
        }

        return parseRevisionToken(req->get("If-Match"));
    }

    template <typename RequestPtr>
    std::string resolveDraftId(const RequestPtr& req, const nlohmann::json* body) {
        if (body != nullptr && body->is_object() && body->contains("draft_id") && (*body).at("draft_id").is_string()) {
            return (*body).at("draft_id").get<std::string>();
        }

        const std::string headerDraftId = req->get("X-Draft-Id");
        if (!headerDraftId.empty()) {
            return headerDraftId;
        }

        return DEFAULT_DRAFT_ID;
    }

    template <typename ResponsePtr>
    void setRevisionHeaders(const ResponsePtr& res, std::uint64_t revision) {
        const std::string revisionString = std::to_string(revision);
        res->set("ETag", "\"rev-" + revisionString + "\"");
        res->set("X-Mapping-Revision", revisionString);
    }

    template <typename ResponsePtr>
    void setLegacyRouteWarningHeaders(const ResponsePtr& res) {
        res->set("Deprecation", "true");
        res->set("Warning", "299 - Legacy /config draft mutation routes are deprecated; prefer /drafts/* endpoints");
    }

    std::optional<int64_t> parseExpectedDraftRevision(const nlohmann::json& body) {
        if (body.is_object() && body.contains("expected_draft_revision") && body["expected_draft_revision"].is_number_integer()) {
            return body["expected_draft_revision"].get<int64_t>();
        }

        return std::nullopt;
    }

    std::optional<int64_t> readCurrentDraftRevisionSafe(const std::string& draftId) {
        try {
            return mqtt::lib::JsonMappingReader::readDraftRevision(draftId);
        } catch (...) {
            return std::nullopt;
        }
    }

    nlohmann::json makeDraftConflictResponse(const std::string& draftId, const std::string& details) {
        nlohmann::json conflictResponse = {{"error", "Draft modified concurrently"}, {"details", details}, {"draft_id", draftId}};

        const std::optional<int64_t> currentDraftRevision = readCurrentDraftRevisionSafe(draftId);
        if (currentDraftRevision.has_value()) {
            conflictResponse["current_draft_revision"] = currentDraftRevision.value();
        }

        return conflictResponse;
    }

    nlohmann::json makeDeployAckResponse(const MappingApplyResult& applyResult, bool includeDraftId) {
        nlohmann::json response = {{"status", applyResult.mappingPersisted ? "persist-ack" : "deploy-ack"},
                                   {"revision", applyResult.revision},
                                   {"reload_mode", applyResult.reloadResult.mode},
                                   {"instances", applyResult.reloadResult.instances},
                                   {"subscribed", applyResult.reloadResult.subscribed},
                                   {"unsubscribed", applyResult.reloadResult.unsubscribed}};
        if (includeDraftId) {
            response["draft_id"] = applyResult.draftId;
        }

        return response;
    }

    template <typename ResponsePtr>
    bool requireExpectedDraftRevision(const nlohmann::json& body, const std::string& endpoint, const ResponsePtr& res, std::optional<int64_t>& outRevision) {
        outRevision = parseExpectedDraftRevision(body);
        if (outRevision.has_value()) {
            return true;
        }

        res->status(428).json({{"error", "Missing expected_draft_revision"},
                               {"details", endpoint + " requires expected_draft_revision to prevent lost updates"}});
        return false;
    }

    template <typename ResponsePtr>
    void respondRevisionConflict(const ResponsePtr& res,
                                 const std::string& details,
                                 const std::function<std::optional<std::uint64_t>()>& currentRevisionProvider) {
        nlohmann::json conflictResponse = {{"error", "Revision conflict"}, {"details", details}};
        try {
            const std::optional<std::uint64_t> currentRevision = currentRevisionProvider();
            if (currentRevision.has_value()) {
                conflictResponse["current_revision"] = currentRevision.value();
            }
        } catch (...) {
        }

        res->status(412).json(conflictResponse);
    }

    template <typename RequestPtr>
    nlohmann::json parseJsonBody(const RequestPtr& req, bool allowEmpty = false) {
        if (allowEmpty && req->body.empty()) {
            return nlohmann::json::object();
        }

        return nlohmann::json::parse(std::string(req->body.begin(), req->body.end()));
    }

    MappingApplyResult applyMappingAndReload(mqtt::lib::ConfigApplication* configApplication,
                                             const nlohmann::json& mapping,
                                             const std::string& draftId,
                                             const mqtt::lib::admin::ReloadCallback& onDeploy) {
        const bool mustReconnect = configApplication->getMqttMapper()->setMapping(mapping);
        const bool mappingPersisted = configApplication->persistMapping();
        mqtt::lib::admin::ReloadResult reloadResult;
        if (onDeploy) {
            reloadResult = onDeploy(mustReconnect);
        }

        return MappingApplyResult{configApplication->getMqttMapper()->getRevision(), draftId, mappingPersisted, reloadResult};
    }

    ActiveState readActiveState(mqtt::lib::ConfigApplication* configApplication) {
        return ActiveState{configApplication->getMqttMapper()->getMapping(), configApplication->getMqttMapper()->getRevision()};
    }

    MappingApplyResult deployDraft(mqtt::lib::ConfigApplication* configApplication,
                                   const std::string& mapFilePath,
                                   const std::string& draftId,
                                   const std::optional<std::uint64_t>& expectedRevision,
                                   const mqtt::lib::admin::ReloadCallback& onDeploy) {
        const ActiveState activeState = readActiveState(configApplication);

        const nlohmann::json newMappingJson = mqtt::lib::JsonMappingReader::deployDraft(
            mapFilePath,
            draftId,
            activeState.mapping,
            activeState.revision,
            expectedRevision);

        return applyMappingAndReload(configApplication, newMappingJson, draftId, onDeploy);
    }

    MappingApplyResult rollbackToVersion(mqtt::lib::ConfigApplication* configApplication,
                                         const std::string& mapFilePath,
                                         const std::string& versionId,
                                         const std::optional<std::uint64_t>& expectedRevision,
                                         const mqtt::lib::admin::ReloadCallback& onDeploy) {
        const ActiveState activeState = readActiveState(configApplication);
        const nlohmann::json rolledbackMappingJson = mqtt::lib::JsonMappingReader::rollbackTo(
            mapFilePath,
            versionId,
            activeState.mapping,
            activeState.revision,
            expectedRevision);

        return applyMappingAndReload(configApplication, rolledbackMappingJson, "", onDeploy);
    }

    template <typename MutationFn>
    void applyDraftMutationWithAutoCreate(mqtt::lib::ConfigApplication* configApplication,
                                          const std::string& draftId,
                                          MutationFn&& mutation) {
        try {
            mutation();
        } catch (const mqtt::lib::EntityNotFoundError&) {
            const ActiveState activeState = readActiveState(configApplication);
            mqtt::lib::JsonMappingReader::createDraftFromMapping(
                activeState.mapping,
                activeState.revision,
                draftId);
            mutation();
        }
    }

    template <typename ResponsePtr>
    void respondDraftValidationResult(const ResponsePtr& res, const std::string& draftId, const nlohmann::json& draftEnvelope) {
        nlohmann::json_schema::basic_error_handler err;
        mqtt::lib::MqttMapper::validate(draftEnvelope.at("mapping"), err);

        if (err) {
            res->status(422).json({{"valid", false}, {"error", "Draft validation failed"}, {"draft_id", draftId}});
            return;
        }

        res->status(200).json({{"valid", true}, {"draft_id", draftId}});
    }

    template <typename RequestPtr, typename ResponsePtr>
    void handleDeployRequest(const RequestPtr& req,
                             const ResponsePtr& res,
                             mqtt::lib::ConfigApplication* configApplication,
                             const std::shared_ptr<AdminMappingContext>& mappingContext,
                             const mqtt::lib::admin::ReloadCallback& onDeploy) {
        try {
            const nlohmann::json body = parseJsonBody(req, true);
            const std::string draftId = resolveDraftId(req, &body);
            const std::optional<std::uint64_t> expectedRevision = resolveExpectedRevision(req, &body);

            const MappingApplyResult applyResult = deployDraft(
                configApplication,
                mappingContext->mappingFilePath,
                draftId,
                expectedRevision,
                onDeploy);

            setRevisionHeaders(res, applyResult.revision);
            res->status(200).json(makeDeployAckResponse(applyResult, true));
        } catch (const nlohmann::json::parse_error& e) {
            res->status(400).json({{"error", "Invalid JSON body"}, {"details", e.what()}});
        } catch (const mqtt::lib::EntityNotFoundError& e) {
            res->status(404).json({{"error", "Draft not found"}, {"details", e.what()}});
        } catch (const mqtt::lib::OCCConflictError& e) {
            respondRevisionConflict(res, e.what(), [&]() -> std::optional<std::uint64_t> {
                return configApplication->getMqttMapper()->getRevision();
            });
        } catch (const std::exception& e) {
            res->status(500).json({{"error", "Deploy failed"}, {"details", e.what()}});
        }
    }

    std::shared_ptr<AdminMappingContext> buildAdminMappingContext() {
        auto context = std::make_shared<AdminMappingContext>();

        namespace fs = std::filesystem;
        const fs::path adminRoot = fs::temp_directory_path() / "mqttsuite" / "admin";
        fs::create_directories(adminRoot);

        context->mappingFilePath = (adminRoot / "active.runtime.json").string();
        return context;
    }

} // namespace

namespace mqtt::lib::admin {

    express::Router makeMappingAdminRouter(ConfigApplication* configApplication, const AdminOptions& opt, ReloadCallback onDeploy) {
        express::Router api;
        const std::shared_ptr<AdminMappingContext> mappingContext = buildAdminMappingContext();

        api.use(express::middleware::JsonMiddleware());
        api.use(express::middleware::BasicAuthentication(opt.user, opt.pass, opt.realm));

        // GET /schema
        api.get("/schema", [] APPLICATION(req, res) {
            res->status(200).send(MqttMapper::getSchema());
        });

        // GET /config (active mapping)
        api.get("/config", [configApplication] APPLICATION(req, res) {
            try {
                const nlohmann::json active = configApplication->getMqttMapper()->getMapping();
                const std::uint64_t revision = configApplication->getMqttMapper()->getRevision();
                setRevisionHeaders(res, revision);

                res->status(200).json(active);
            } catch (const std::exception& e) {
                res->status(500).json({{"error", "Failed to load configuration"}, {"details", e.what()}});
            }
        });

        // POST /drafts/create
        api.post("/drafts/create", [configApplication, mappingContext] APPLICATION(req, res) {
            try {
                nlohmann::json body = parseJsonBody(req, true);
                const ActiveState activeState = readActiveState(configApplication);

                const std::string draftId = JsonMappingReader::createDraftFromMapping(
                    activeState.mapping,
                    activeState.revision,
                    (body.is_object() && body.contains("draft_id") && body["draft_id"].is_string()) ? body["draft_id"].get<std::string>() : "");

                const nlohmann::json draft = JsonMappingReader::readDraft(draftId);
                res->status(201).json(draft);
            } catch (const OCCConflictError& e) {
                res->status(409).json({{"error", "Draft already exists"}, {"details", e.what()}});
            } catch (const std::exception& e) {
                res->status(400).json({{"error", "Draft creation failed"}, {"details", e.what()}});
            }
        });

        // GET /drafts/list
        api.get("/drafts/list", [mappingContext] APPLICATION(req, res) {
            try {
                res->status(200).json(JsonMappingReader::listDrafts());
            } catch (const std::exception& e) {
                res->status(500).json({{"error", "Failed to list drafts"}, {"details", e.what()}});
            }
        });

        // POST /drafts/get
        api.post("/drafts/get", [mappingContext] APPLICATION(req, res) {
            try {
                const nlohmann::json body = parseJsonBody(req);
                const std::string draftId = resolveDraftId(req, &body);
                res->status(200).json(JsonMappingReader::readDraft(draftId));
            } catch (const EntityNotFoundError& e) {
                res->status(404).json({{"error", "Draft not found"}, {"details", e.what()}});
            } catch (const std::exception& e) {
                res->status(400).json({{"error", "Failed to load draft"}, {"details", e.what()}});
            }
        });

        // PATCH /drafts/patch
        api.patch("/drafts/patch", [mappingContext] APPLICATION(req, res) {
            std::string draftId = DEFAULT_DRAFT_ID;
            try {
                const nlohmann::json body = parseJsonBody(req);
                if (!body.is_object() || !body.contains("patch")) {
                    res->status(400).json({{"error", "Body must be an object containing patch"}});
                    return;
                }

                std::optional<int64_t> expectedDraftRevision;
                if (!requireExpectedDraftRevision(body, "PATCH /drafts/patch", res, expectedDraftRevision)) {
                    return;
                }

                draftId = resolveDraftId(req, &body);
                const nlohmann::json draft = JsonMappingReader::patchDraft(draftId, body["patch"], expectedDraftRevision);
                res->status(200).json(draft);
            } catch (const EntityNotFoundError& e) {
                res->status(404).json({{"error", "Draft not found"}, {"details", e.what()}});
            } catch (const OCCConflictError& e) {
                res->status(412).json(makeDraftConflictResponse(draftId, e.what()));
            } catch (const std::exception& e) {
                res->status(422).json({{"error", "Draft patch failed"}, {"details", e.what()}});
            }
        });

        // POST /drafts/replace
        api.post("/drafts/replace", [mappingContext] APPLICATION(req, res) {
            std::string draftId = DEFAULT_DRAFT_ID;
            try {
                const nlohmann::json body = parseJsonBody(req);
                if (!body.is_object() || !body.contains("mapping")) {
                    res->status(422).json({{"error", "Body must contain mapping"}});
                    return;
                }

                std::optional<int64_t> expectedDraftRevision;
                if (!requireExpectedDraftRevision(body, "POST /drafts/replace", res, expectedDraftRevision)) {
                    return;
                }

                draftId = resolveDraftId(req, &body);
                const nlohmann::json draft = JsonMappingReader::replaceDraft(draftId, body["mapping"], expectedDraftRevision);
                res->status(200).json(draft);
            } catch (const EntityNotFoundError& e) {
                res->status(404).json({{"error", "Draft not found"}, {"details", e.what()}});
            } catch (const OCCConflictError& e) {
                res->status(412).json(makeDraftConflictResponse(draftId, e.what()));
            } catch (const std::exception& e) {
                res->status(422).json({{"error", "Draft replacement failed"}, {"details", e.what()}});
            }
        });

        // POST /drafts/validate
        api.post("/drafts/validate", [mappingContext] APPLICATION(req, res) {
            try {
                const nlohmann::json body = parseJsonBody(req);
                const std::string draftId = resolveDraftId(req, &body);

                const nlohmann::json draft = JsonMappingReader::readDraft(draftId);
                respondDraftValidationResult(res, draftId, draft);
            } catch (const EntityNotFoundError& e) {
                res->status(404).json({{"valid", false}, {"error", "Draft not found"}, {"details", e.what()}});
            } catch (const std::exception& e) {
                res->status(400).json({{"valid", false}, {"error", "Draft validation exception"}, {"details", e.what()}});
            }
        });

        // POST /drafts/deploy
        api.post("/drafts/deploy", [configApplication, mappingContext, onDeploy] APPLICATION(req, res) {
            handleDeployRequest(req, res, configApplication, mappingContext, onDeploy);
        });

        // POST /drafts/delete
        api.post("/drafts/delete", [mappingContext] APPLICATION(req, res) {
            try {
                const nlohmann::json body = parseJsonBody(req);
                const std::string draftId = resolveDraftId(req, &body);
                JsonMappingReader::discardDraft(draftId);
                res->status(200).json({{"status", "deleted"}, {"draft_id", draftId}});
            } catch (const std::exception& e) {
                res->status(400).json({{"error", "Draft delete failed"}, {"details", e.what()}});
            }
        });

        // PATCH /config (legacy default draft)
        api.patch("/config", [configApplication, mappingContext] APPLICATION(req, res) {
            try {
                const nlohmann::json patchOps = parseJsonBody(req);

                const std::string draftId = DEFAULT_DRAFT_ID;
                setLegacyRouteWarningHeaders(res);

                applyDraftMutationWithAutoCreate(configApplication, draftId, [&]() {
                    JsonMappingReader::patchDraft(draftId, patchOps);
                });

                res->status(200).json({{"status", "patched"}, {"draft_id", draftId}, {"path", mappingContext->mappingFilePath}});
            } catch (const nlohmann::json::parse_error& e) {
                res->status(400).json({{"error", "Invalid JSON body"}, {"details", e.what()}});
            } catch (const std::exception& e) {
                res->status(422).json({{"error", "Patch application failed"}, {"details", e.what()}});
            }
        });

        // POST /config (legacy replace full draft)
        api.post("/config", [configApplication, mappingContext] APPLICATION(req, res) {
            try {
                nlohmann::json replacement = parseJsonBody(req);

                const std::string draftId = DEFAULT_DRAFT_ID;
                setLegacyRouteWarningHeaders(res);

                applyDraftMutationWithAutoCreate(configApplication, draftId, [&]() {
                    JsonMappingReader::replaceDraft(draftId, replacement);
                });

                res->status(200).json({{"status", "replaced"}, {"draft_id", draftId}, {"path", mappingContext->mappingFilePath}});
            } catch (const nlohmann::json::parse_error& e) {
                res->status(400).json({{"error", "Invalid JSON body"}, {"details", e.what()}});
            } catch (const std::exception& e) {
                res->status(422).json({{"error", "Config replacement failed"}, {"details", e.what()}});
            }
        });

        // POST /config/deploy (legacy wrapper)
        api.post("/config/deploy", [configApplication, mappingContext, onDeploy] APPLICATION(req, res) {
            handleDeployRequest(req, res, configApplication, mappingContext, onDeploy);
        });

        // POST /config/validate
        api.post("/config/validate", [] APPLICATION(req, res) {
            try {
                auto document = parseJsonBody(req);

                nlohmann::json_schema::basic_error_handler err;
                MqttMapper::validate(document, err);

                if (err) {
                    res->status(422).json({{"valid", false}, {"error", "Validation failed"}});
                } else {
                    res->status(200).json({{"valid", true}});
                }
            } catch (const std::exception& e) {
                res->status(400).json({{"error", "Validation exception"}, {"details", e.what()}});
            }
        });

        // GET /config/validateDraft (legacy wrapper)
        api.get("/config/validateDraft", [mappingContext] APPLICATION(req, res) {
            try {
                const std::string draftId = DEFAULT_DRAFT_ID;
                setLegacyRouteWarningHeaders(res);
                const nlohmann::json draft = JsonMappingReader::readDraft(draftId);
                respondDraftValidationResult(res, draftId, draft);
            } catch (const EntityNotFoundError& e) {
                res->status(404).json({{"valid", false}, {"error", "No draft configuration available"}, {"details", e.what()}});
            } catch (const std::exception& e) {
                res->status(400).json({{"valid", false}, {"error", "Draft validation exception"}, {"details", e.what()}});
            }
        });

        // POST /config/rollback
        api.post("/config/rollback", [configApplication, mappingContext, onDeploy] APPLICATION(req, res) {
            try {
                auto jsonBody = parseJsonBody(req);

                if (!jsonBody.contains("version_id")) {
                    res->status(400).json({{"error", "Missing version_id"}});
                    return;
                }

                std::string versionId = jsonBody["version_id"];
                const std::optional<std::uint64_t> expectedRevision = resolveExpectedRevision(req, &jsonBody);

                const MappingApplyResult applyResult = rollbackToVersion(
                    configApplication,
                    mappingContext->mappingFilePath,
                    versionId,
                    expectedRevision,
                    onDeploy);

                setRevisionHeaders(res, applyResult.revision);
                res->status(200).json(makeDeployAckResponse(applyResult, false));
            } catch (const nlohmann::json::parse_error& e) {
                res->status(400).json({{"error", "Invalid JSON body"}, {"details", e.what()}});
            } catch (const OCCConflictError& e) {
                respondRevisionConflict(res, e.what(), [&]() -> std::optional<std::uint64_t> {
                    return configApplication->getMqttMapper()->getRevision();
                });
            } catch (const EntityNotFoundError& e) {
                res->status(404).json({{"error", "Version not found"}, {"details", e.what()}});
            } catch (const std::exception& e) {
                res->status(500).json({{"error", "Rollback failed"}, {"details", e.what()}});
            }
        });

        // GET /config/history
        api.get("/config/history", [mappingContext] APPLICATION(req, res) {
            try {
                auto history = JsonMappingReader::getHistory(mappingContext->mappingFilePath);
                nlohmann::json list = nlohmann::json::array();
                for (const auto& h : history) {
                    list.push_back({{"id", h.id}, {"comment", h.comment}, {"date", h.date}});
                }
                res->status(200).json(list);
            } catch ([[maybe_unused]] const std::exception& e) {
                res->status(500).json({{"error", "Failed to fetch history"}});
            }
        });

        api.get("/", [] APPLICATION(req, res) {
            res->redirect("/ui");
        });

        api.get("/ui", [] APPLICATION(req, res) {
            res->redirect("/ui/index.html");
        });

        api.use("/ui",
                express::middleware::StaticMiddleware("/home/voc/tmp/integrator/mqtt-integrator-ui/dist/mqtt-integrator-ui/browser"));

        api.get("*", [] APPLICATION(req, res) {
            res->redirect("/ui/index.html");
        });

        return api;
    }

} // namespace mqtt::lib::admin
