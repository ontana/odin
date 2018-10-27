/**
    Copyright 2018 Felspar Co Ltd. <http://odin.felspar.com/>

    Distributed under the Boost Software License, Version 1.0.
    See <http://www.boost.org/LICENSE_1_0.txt>
*/

#include <odin/app.hpp>
#include <odin/credentials.hpp>
#include <odin/group.hpp>
#include <odin/nonce.hpp>
#include <odin/odin.hpp>
#include <odin/user.hpp>
#include <odin/views.hpp>

#include <fost/exception/parse_error.hpp>
#include <fost/insert>
#include <fost/log>
#include <fostgres/sql.hpp>


namespace {

    const fostlib::module c_odin_app(odin::c_odin, "app.cpp");

    fostlib::json parse_payload(fostlib::http::server::request &req) {
        // TODO: Support multiple ContentType
        auto body_str = fostlib::coerce<fostlib::string>(
            fostlib::coerce<fostlib::utf8_string>(req.data()->data()));
        return fostlib::json::parse(body_str);
    }

    const class app_login : public fostlib::urlhandler::view {
    public:
        app_login()
        : view("odin.app.login") {
        }

        std::pair<boost::shared_ptr<fostlib::mime>, int> operator () (
            const fostlib::json &config,
            const fostlib::string &path,
            fostlib::http::server::request &req,
            const fostlib::host &host
        ) const {
            const auto paths = fostlib::split(path, "/");
            if ( paths.size() != 1)
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "Must pass app_id in the URL");
            fostlib::pg::connection cnx{fostgres::connection(config, req)};
            const auto app_id = paths[0];
            fostlib::json app = odin::app::get_detail(cnx, app_id);
            cnx.commit();
            if ( app.isnull() ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__, "App not found");
            }

            if ( fostlib::coerce<fostlib::string>(app["app"]["access_policy"]) != "INVITE_ONLY" ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "Invalid access policy, supported only INVITE_ONLY");
            }

            if ( req.method() == "GET" ) {
                boost::filesystem::wpath filename(
                    fostlib::coerce<boost::filesystem::wpath>(config["static"]));
                return fostlib::urlhandler::serve_file(config, req, filename);
            }
            if ( req.method() != "POST" )
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "App Login required POST, this should be a 405");

            fostlib::json body = parse_payload(req);
            if ( !body.has_key("username") || !body.has_key("password"))
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "Must pass both username and password fields");
            const auto username = fostlib::coerce<fostlib::string>(body["username"]);
            const auto password = fostlib::coerce<fostlib::string>(body["password"]);


            auto user = odin::credentials(cnx, username, password, req.remote_address());
            if ( user.isnull() ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__, "User not found");
            }

            auto app_user = odin::app::get_app_user(cnx, app_id, fostlib::coerce<f5::u8view>(user["identity"]["id"]));
            if ( app_user.isnull() ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__, "Forbidden");
            }

            auto jwt = odin::app::mint_user_jwt(user, app);
            fostlib::mime::mime_headers headers;
            auto exp = jwt.expires(fostlib::coerce<fostlib::timediff>(config["expires"]), false);
            headers.add("Expires", fostlib::coerce<fostlib::rfc1123_timestamp>(exp).underlying().underlying().c_str());

            const auto jwt_token = fostlib::utf8_string(jwt.token());
            const auto redirect_url = fostlib::coerce<fostlib::string>(app["app"]["redirect_url"]);

            fostlib::json result;
            fostlib::insert(result, "access_token", jwt_token);
            fostlib::insert(result, "scheme", "Bearer");
            fostlib::insert(result, "redirect_url", redirect_url);
            boost::shared_ptr<fostlib::mime> response(
                new fostlib::text_body(fostlib::json::unparse(result, true), headers, L"application/json"));
            return std::make_pair(response, 200);
        }

    } c_app_login;


    const class app_handover : public fostlib::urlhandler::view {
    public:
        app_handover()
        : view("odin.app.handover") {
        }

        std::pair<boost::shared_ptr<fostlib::mime>, int> operator () (
            const fostlib::json &config,
            const fostlib::string &path,
            fostlib::http::server::request &req,
            const fostlib::host &host
        ) const {
            const auto app_id = fostlib::coerce<fostlib::string>(config["app_id"]);
            if ( req.method() != "POST" )
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "App Handover required POST, this should be a 405");

            fostlib::json body = parse_payload(req);
            if ( !body.has_key("token") )
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "Must pass token field");
            fostlib::json fed_response_data{};
            if ( config.has_key("mock") ) {
                fostlib::json mock_identity{};
                fostlib::insert(mock_identity, "id", "mock_user");
                fostlib::insert(mock_identity, "email", "mock_user@email.com");
                fostlib::insert(mock_identity, "full_name", "Mock User");
                fostlib::insert(fed_response_data, "identity", mock_identity);
                fostlib::insert(fed_response_data, "roles", fostlib::json::parse( L"[\"admin-group\", \"admin-user\"]" ));
            } else {
                fostlib::url federation_url(fostlib::coerce<fostlib::string>(config["federation_url"]));
                fostlib::http::user_agent ua{};
                boost::shared_ptr<fostlib::mime> fed_data(
                    new fostlib::text_body(fostlib::json::unparse(body, true),
                        fostlib::mime::mime_headers(), L"application/json"));
                auto fed_resp = ua.post(federation_url, fed_data);
                fed_response_data = fostlib::json::parse(fostlib::coerce<fostlib::string>(fostlib::coerce<fostlib::utf8_string>(fed_resp->body()->data())));
            }
            // Create user
            fostlib::pg::connection cnx{fostgres::connection(config, req)};
            auto ref = odin::reference();
            auto const identity_id = fostlib::coerce<fostlib::string>(fed_response_data["identity"]["id"]);
            odin::create_user(cnx, ref, identity_id);

            if ( odin::is_module_enabled(cnx, "opts/full-name") ){
                odin::set_full_name(cnx, ref, identity_id,
                    fostlib::coerce<fostlib::string>(fed_response_data["identity"]["full_name"]));
            }

            if ( odin::is_module_enabled(cnx, "opts/email") ){
                odin::set_email(cnx, ref, identity_id,
                    fostlib::coerce<fostlib::email_address>(fed_response_data["identity"]["email"]));
            }

            // Add membership to group
            if ( odin::is_module_enabled(cnx, "authz") ){
                for ( const auto &group : fed_response_data["roles"] ) {
                    odin::group::add_membership(cnx, ref, identity_id,
                        fostlib::coerce<fostlib::string>(group));
                }
            }

            cnx.commit();
            auto jwt(odin::mint_login_jwt(fed_response_data));
            auto exp = jwt.expires(fostlib::coerce<fostlib::timediff>(config["expires"]), false);

            fostlib::mime::mime_headers headers;
            headers.add("Expires", fostlib::coerce<fostlib::rfc1123_timestamp>(exp).underlying().underlying().c_str());
            boost::shared_ptr<fostlib::mime> response(
                            new fostlib::text_body(fostlib::utf8_string(jwt.token()),
                                headers, L"application/jwt"));
            return std::make_pair(response, 200);
        }

    } c_app_handover;


    const class app_verify : public fostlib::urlhandler::view {
    public:
        app_verify()
        : view("odin.app.verify") {
        }

        std::pair<boost::shared_ptr<fostlib::mime>, int> operator () (
            const fostlib::json &config,
            const fostlib::string &path,
            fostlib::http::server::request &req,
            const fostlib::host &host
        ) const {
            const auto paths = fostlib::split(path, "/");
            if ( paths.size() != 1) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "Must pass app_id in the URL");
            }
            fostlib::pg::connection cnx{fostgres::connection(config, req)};
            const auto app_id = paths[0];
            fostlib::json app = odin::app::get_detail(cnx, app_id);
            if ( app.isnull() ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__, "App not found");
            }

            if ( req.method() != "POST" ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "App Login required POST, this should be a 405");
            }
            fostlib::json body = parse_payload(req);
            if ( !body.has_key("token") ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "Must pass token field");
            }
            const auto user_token = fostlib::coerce<fostlib::string>(body["token"]);
            const fostlib::string jwt_secret = odin::c_jwt_secret.value()
                + fostlib::coerce<fostlib::string>(app["app"]["app_id"]);

            auto jwt = fostlib::jwt::token::load(jwt_secret, user_token);
            if ( not jwt ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__, "JWT unauthenticated");
            }
            auto const iss = fostlib::coerce<fostlib::string>(jwt.value().payload["iss"]);
            if ( iss != app_id ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__, "app_id mismatch");
            }

            fostlib::log::debug(odin::c_odin)
                ("", "Hands over JWT authenticated")
                ("header", jwt.value().header)
                ("payload", jwt.value().payload);

            static const fostlib::string sql("SELECT "
                "odin.identity.tableoid AS identity__tableoid, "
                "odin.identity.*, "
                "(SELECT COALESCE(json_agg(role), '[]'::json) "
                "FROM odin.app_user_role "
                "WHERE odin.app_user_role.app_id= $1 "
                "AND odin.app_user_role.identity_id= $2) as roles "
            "FROM odin.identity "
            "INNER JOIN odin.app_user ON odin.identity.id = odin.app_user.identity_id "
            "WHERE odin.identity.id = $2 "
            "AND odin.app_user.app_id = $1"
            );
            const auto user_id = fostlib::coerce<fostlib::string>(jwt.value().payload["sub"]);
            auto data = fostgres::sql(cnx, sql, std::vector<fostlib::string>{app_id, user_id});
            auto &rs = data.second;
            auto row = rs.begin();

            if ( row == rs.end() ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__, "App user not found");
            }

            auto record = *row;
            if ( ++row != rs.end() ) {
                throw fostlib::exceptions::not_implemented(__PRETTY_FUNCTION__,
                    "More than one user returned");
            }

            fostlib::json user;
            for ( std::size_t index{0}; index < record.size(); ++index ) {
                const auto parts = fostlib::split(data.first[index], "__");
                if ( parts.size() && parts[parts.size() - 1] == "tableoid" ) {
                    continue;
                }
                fostlib::jcursor pos;
                for ( const auto &p : parts ) pos /= p;
                fostlib::insert(user, pos, record[index]);
            }
            fostlib::mime::mime_headers headers;
            boost::shared_ptr<fostlib::mime> response(
                new fostlib::text_body(fostlib::json::unparse(user, true), headers, L"application/json"));
            return std::make_pair(response, 200);
        }

    } c_app_verify;


}


const fostlib::urlhandler::view &odin::view::app_login = c_app_login;

