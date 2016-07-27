/*
    Copyright 2016 Felspar Co Ltd. http://odin.felspar.com/
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include <odin/odin.hpp>
#include <odin/views.hpp>

#include <fost/crypto>
#include <fost/insert>


namespace {


    const class secure : public fostlib::urlhandler::view {
    public:
        secure()
        : view("odin.secure") {
        }

        std::pair<boost::shared_ptr<fostlib::mime>, int> operator () (
            const fostlib::json &config, const fostlib::string &path,
            fostlib::http::server::request &req,
            const fostlib::host &host
        ) const {
            if ( req.headers().exists("Authorization") ) {
                auto parts = fostlib::partition(req.headers()["Authorization"].value(), " ");
                if ( parts.first == "Bearer" && not parts.second.isnull() ) {
                    auto jwt = fostlib::jwt::token::load(
                        odin::c_jwt_secret.value(), parts.second.value());
                    if ( not jwt.isnull() ) {
                        return execute(config["secure"], path, req, host);
                    }
                }
            }
            return execute(config["unsecure"], path, req, host);
        }
    } c_secure;


}


const fostlib::urlhandler::view &odin::view::secure = c_secure;
