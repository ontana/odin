/**
    Copyright 2019 Felspar Co Ltd. <http://odin.felspar.com/>

    Distributed under the Boost Software License, Version 1.0.
    See <http://www.boost.org/LICENSE_1_0.txt>
*/


#include <odin/app.hpp>
#include <odin/google.hpp>
#include <odin/nonce.hpp>
#include <odin/user.hpp>
#include <odin/views.hpp>

#include <fost/insert>
#include <fost/log>
#include <fost/push_back>
#include <fostgres/sql.hpp>

#include <pqxx/except>


namespace {


    fostlib::module const c_odin_app_google{odin::c_odin_app, "google"};


    inline std::pair<boost::shared_ptr<fostlib::mime>, int>
            respond(fostlib::string message, int code = 403) {
        fostlib::json ret;
        if (not message.empty())
            fostlib::insert(ret, "message", std::move(message));
        fostlib::mime::mime_headers headers;
        boost::shared_ptr<fostlib::mime> response(new fostlib::text_body(
                fostlib::json::unparse(ret, true), headers, "application/json"));
        return std::make_pair(response, code);
    }


    const class g_login : public fostlib::urlhandler::view {
      public:
        g_login() : view("odin.app.google.login") {}

        std::pair<boost::shared_ptr<fostlib::mime>, int> operator()(
                const fostlib::json &config,
                const fostlib::string &path,
                fostlib::http::server::request &req,
                const fostlib::host &host) const {
            auto logger{fostlib::log::debug(c_odin_app_google)};
            if (req.method() != "POST") {
                fostlib::json config;
                fostlib::insert(config, "view", "fost.response.405");
                fostlib::push_back(config, "configuration", "allow", "POST");
                return execute(config, path, req, host);
            }
            if (not req.headers().exists("__app")
                || not req.headers().exists("__user")) {
                throw fostlib::exceptions::not_implemented(
                        __PRETTY_FUNCTION__,
                        "The odin.app.google.login view must be wrapped by an "
                        "odin.app.secure view on the secure path so that there "
                        "is a valid JWT to find the App ID in");
            }
            logger("__app", req.headers()["__app"]);
            logger("__user", req.headers()["__user"]);

            auto body_str = fostlib::coerce<fostlib::string>(
                    fostlib::coerce<fostlib::utf8_string>(req.data()->data()));
            fostlib::json body = fostlib::json::parse(body_str);
            logger("body", body);

            if (not body.has_key("access_token"))
                throw fostlib::exceptions::not_implemented(
                        "odin.app.google.login",
                        "Must pass access_token field");
            auto const access_token =
                    fostlib::coerce<fostlib::string>(body["access_token"]);

            fostlib::json user_detail;
            if (config.has_key("google-mock")) {
                if (fostlib::coerce<fostlib::string>(config["google-mock"])
                    == "OK") {
                    // Use access token as google ID
                    fostlib::insert(user_detail, "sub", access_token);
                    fostlib::insert(user_detail, "name", "Test User");
                    fostlib::insert(
                            user_detail, "email",
                            access_token + "@example.com");
                }
            } else {
                user_detail = odin::google::get_user_detail(access_token);
            }
            logger("user_detail", user_detail);
            if (user_detail.isnull())
                throw fostlib::exceptions::not_implemented(
                        "odin.app.google.login", "User not authenticated");

            auto const google_user_id =
                    fostlib::coerce<f5::u8view>(user_detail["sub"]);
            fostlib::pg::connection cnx{fostgres::connection(config, req)};
            auto const reference = odin::reference();
            auto google_user = odin::google::credentials(cnx, google_user_id);
            logger("google_user", google_user);

            fostlib::string identity_id;
            if (google_user.isnull()) {
                /// We've never seen this Google identity before,
                /// we take as a new user registration. If this is a new
                /// installation then this is a new user registration, if
                /// the JWT represents an existing user then this links
                /// their Google a/c to their pre-existing identity.
                identity_id = req.headers()["__user"].value();
                if (user_detail.has_key("name")) {
                    const auto google_user_name =
                            fostlib::coerce<f5::u8view>(user_detail["name"]);
                    odin::set_full_name(
                            cnx, reference, identity_id, google_user_name);
                }
                if (user_detail.has_key("email")) {
                    const auto google_user_email =
                            fostlib::coerce<fostlib::email_address>(
                                    user_detail["email"]);
                    if (odin::does_email_exist(
                                cnx,
                                fostlib::coerce<fostlib::string>(
                                        user_detail["email"]))) {
                        return respond("This email already exists", 422);
                    }
                    odin::set_email(
                            cnx, reference, identity_id, google_user_email);
                }
                odin::google::set_google_credentials(
                        cnx, reference, identity_id, google_user_id);
                google_user = odin::google::credentials(cnx, google_user_id);
                cnx.commit();
            } else if (
                    google_user["identity"]["id"]
                    == req.headers()["__user"].value()) {
                /// Not sure what to do here. Certainly OK for now.
                /// Probably should allow updates of email and name
                identity_id = req.headers()["__user"].value();
            } else {
                /// An existing user has logged in to a new device. Probably
                /// there are two cases here:
                /// 1. The user doesn't already have an account on this app.
                /// 2. The user does already have an account on this app.
                identity_id = fostlib::coerce<fostlib::string>(
                        google_user["identity"]["id"]);
                fostlib::json merge;
                fostlib::insert(
                        merge, "from_identity_id",
                        req.headers()["__user"].value());
                fostlib::insert(
                        merge, "to_identity_id", google_user["identity"]["id"]);
                fostlib::insert(
                        merge, "annotation", "app", req.headers()["__app"]);
                try {
                    /// Case 1 above
                    cnx.insert("odin.merge_ledger", merge);
                    cnx.commit();
                } catch (const pqxx::unique_violation &e) {
                    /// We replace the identity with the new one -- case 2 above
                } catch (...) {
                    throw;
                }
            }

            auto jwt = odin::app::mint_user_jwt(
                    identity_id, req.headers()["__app"].value(),
                    fostlib::coerce<fostlib::timediff>(config["expires"]));
            fostlib::mime::mime_headers headers;
            headers.add(
                    "Expires",
                    fostlib::coerce<fostlib::rfc1123_timestamp>(jwt.second)
                            .underlying()
                            .underlying()
                            .c_str());
            boost::shared_ptr<fostlib::mime> response(new fostlib::text_body(
                    jwt.first, headers, L"application/jwt"));

            return std::make_pair(response, 200);
        }
    } c_g_login;


}
