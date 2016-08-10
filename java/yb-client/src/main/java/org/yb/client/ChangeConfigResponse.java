// Copyright (c) YugaByte, Inc.

package org.yb.client;

import org.yb.annotations.InterfaceAudience;
import org.yb.tserver.Tserver.TabletServerErrorPB;

@InterfaceAudience.Public
public class ChangeConfigResponse extends YRpcResponse {
  private TabletServerErrorPB serverError;

  ChangeConfigResponse(long ellapsedMillis, String masterUUID, TabletServerErrorPB error) {
    super(ellapsedMillis, masterUUID);
    serverError = error;
  }

  public boolean hasError() {
    return serverError != null;
  }

  public String errorMessage() {
    if (serverError == null) {
      return "";
    }

    return serverError.getStatus().getMessage();
  }
}