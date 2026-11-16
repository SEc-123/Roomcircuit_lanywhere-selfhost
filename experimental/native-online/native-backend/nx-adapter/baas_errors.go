package main

import (
	"net/http"

	"roomcircuit.local/nx-adapter/wire"
)

func (a *adapter) baasFailure(w http.ResponseWriter, r *http.Request, status int, code string) {
	title, detail := http.StatusText(status), "The account request could not be completed."
	switch code {
	case "invalid_params":
		title, detail = "Invalid Params", "The request parameters are invalid."
	case "invalid_device_account":
		title, detail = "Invalid Device Account", "The device account ID or password is invalid."
	case "invalid_ndas_app_authn_token":
		title, detail = "Invalid NDAS App AuthN Token", "The title authorization is invalid for this enrolled device."
	case "invalid_idp":
		title, detail = "Invalid Identity Provider", "The requested account provider is unavailable."
	case "invalid_idp_account":
		title, detail = "Invalid IdP Account", "The account identity could not be verified."
	case "invalid_token":
		title, detail = "Token is invalid", "The access token was invalid."
	case "insufficient_scope":
		title, detail = "Token is insufficient", "The access token does not have sufficient scope."
	case "membership_required":
		title, detail = "Membership Required", "This account has no active game grant."
	case "linked_user_not_found":
		title, detail = "Linked User Not Found", "Link an account on this device before signing in."
	case "under_maintenance":
		detail = "The account service is temporarily unavailable."
	case "resource_is_not_found":
		title, detail = "Specified resource is not found", "Resource is not found."
	case "invalid_raw_content":
		title, detail = "Invalid Raw Content", "The image could not be read."
	}
	instance := "/1.0.0"
	switch r.URL.Path {
	case "/1.0.0/application/token", "/1.0.0/users", "/1.0.0/login", "/1.0.0/federation", "/1.0.0/image_upload":
		instance = r.URL.Path
	default:
		if id := r.PathValue("id"); validNativeUserID(id) && r.URL.Path == "/1.0.0/users/"+id {
			instance = r.URL.Path
		}
	}
	problem, err := wire.BuildBaaSError(a.issuer, status, code, title, detail, instance)
	if err != nil {
		problem, _ = wire.BuildBaaSError(a.issuer, status, "invalid_request", http.StatusText(status), detail, "/1.0.0")
	}
	respond(w, status, problem)
}
